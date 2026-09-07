# Write-Ahead Logging and Recovery

> Write down what you are about to do, flush it, then do it. If you crash, you can finish or discard. If you do it first and write it down after, you can crash in between and never know it happened.

**Learned during:** Sententia Phase 5, fault-tolerant recovery
**Tags:** `durability` `write-ahead-log` `fsync` `snapshots` `crash-recovery` `storage`

---

## The problem it solves

Through four phases my order book lived entirely in memory. Replication
meant a second copy of it in a second process's memory, which is genuinely
useful right up until the power goes out in the building, at which point
the venue is gone.

So state has to reach disk. The naive version is obvious and wrong:

```
apply the order to the book
write it to a file
```

Crash between those two lines and the order happened, was possibly
reported to a client, and left no trace. On restart the book is missing a
trade that somebody was told about.

Flip the order and the failure changes character completely:

```
write it to a file, and flush
apply the order to the book
```

Now a crash between the lines leaves an order on disk that was not
applied. On restart you replay it, and you are correct. A crash before
the write leaves nothing, and nobody was told anything, so you are also
correct.

That asymmetry is the whole idea. One ordering has a window where you
lose acknowledged work. The other has a window where you redo unfinished
work, which is harmless when the work is deterministic.

## The core idea

Two rules and one file.

**Rule one: write ahead.** The record reaches durable storage before the
action it describes is treated as done. "Done" means whatever your system
means by it: committed, acknowledged, visible to a client.

**Rule two: recovery is replay.** On restart, read the log and apply
everything in it. Not a special reconstruction path, just the ordinary
apply loop with a different input.

Rule two only works if applying the same records twice, or applying them
in a fresh process, gives the same answer. Which is to say: your state
machine has to be deterministic. In Sententia that was established in
Phase 1 for entirely different reasons, and it is the fourth time that
one property has paid for itself.

## How it works

Each record is framed with its length and a checksum:

```
 offset  size  field
 0       4     magic
 4       4     seq
 8       4     length
 12      4     crc32 of the payload
 16      ...   payload
```

The checksum is not paranoia about disks lying. It is about **torn
writes**. A crash partway through a write leaves a record that is
physically half there. The file is not corrupt in any way you would
notice: it just ends in a header with no payload behind it, or a payload
that stops early. Read it back without checking and you get garbage
shaped exactly like data.

So recovery replays records until one fails to verify, then truncates the
file at that point. A torn tail is the **expected** outcome of a crash,
not an error condition. That record was never flushed, so it was never
acknowledged, so discarding it is right.

Everything after a bad record goes too, not just the bad one. Once a
record fails to verify there is no basis for trusting what follows it.

## The key insight

The insight that reframed this for me is that **fsync is the entire
subject**, and everything else is bookkeeping around it.

`write()` does not put bytes on a disk. It puts them in the kernel's page
cache and returns. The data reaches the platter whenever the OS feels
like it, which might be thirty seconds later, and a power cut in between
loses it. `write()` returning success means the kernel accepted
responsibility, not that the data is safe.

`fsync()` is what actually makes the promise. It is also, by a wide
margin, the most expensive thing in the write path.

So the durability guarantee is not a property of your log format. It is a
direct function of how often you call fsync, and that is a dial:

| Policy | fsyncs per 100 records | A power cut loses |
|--------|------------------------|-------------------|
| Every write | 100 | nothing acknowledged |
| Batched, N=10 | 10 | up to 9 acknowledged records |
| Never | 0 | everything not yet flushed |

Measured directly in my tests, not quoted from anywhere.

This is the same *shape* of decision as synchronous versus asynchronous
replication, and noticing that was the useful part. Both are "how much do
you pay in the critical path for how strong a promise". Replication trades
latency for surviving a node dying; fsync trades latency for surviving
the building losing power. Most production systems batch both, and both
choices deserve to be stated in terms of what a customer loses rather
than in terms of milliseconds.

The second insight is smaller and cost me an hour when I first met it
years ago: **renaming a file atomically is not enough to make it
durable.** Rename within a directory is atomic on POSIX, so writing to a
temp file and renaming over the target means a crash leaves either the
whole old file or the whole new one. But the rename itself only becomes
durable once the **directory entry** reaches disk, which needs an fsync
on the directory. Skip it and everything passes every test, and a real
power cut loses the rename.

## The tradeoffs

**Every write happens twice.** Once to the log, once to the actual state.
Write amplification is inherent, and it is why databases care so much
about making the log write sequential and the state write lazy.

**The log grows forever unless something truncates it,** and truncating
it is what creates the need for snapshots, which create their own
problems. Durability is a chain of consequences, not a single feature.

**Recovery time grows with the log.** A log with fifty million records
means fifty million records to replay before the node is useful. Bounded
only by snapshotting.

**fsync latency is not under your control.** It depends on the device,
the filesystem, whether a write cache is enabled and honest about it, and
what else is happening on the machine. A benchmark number for fsync is a
number for that machine on that day.

**You now have a file format,** and file formats are forever. Anything
you fail to record cannot be recovered later, and anything you record
badly you will be parsing for years.

## Snapshots, and the gap they create

A snapshot is the state as of some sequence number. Recovery loads the
newest one and replays only what came after, so recovery time becomes a
function of time since the last snapshot rather than of total history.

Snapshotting a matching engine taught me something I would have got
wrong. It is not enough to capture the visible book. Sententia's engine
also holds an arrival counter that breaks ties for time priority, and
leaving it out of the snapshot restores a book that looks completely
correct and quietly gets time priority wrong for every order placed
afterwards. The kind of bug that shows up weeks later as unfair fills and
is nearly impossible to trace back.

There is also an ordering question with a genuinely interesting answer.
When you snapshot, do you write the snapshot first or truncate the log
first?

Snapshot first. If you truncate first and crash before the snapshot is
written, the log is gone and the snapshot does not exist, and you have
lost everything.

But that ordering has a cost, and the cost is where the bugs live. A
crash **between** writing the snapshot and truncating the log leaves a
snapshot covering a prefix and a log that still contains that entire
prefix. Recovery then sees every entry in the overlap twice, once inside
the snapshot and once in the log, and has to apply exactly the ones after
the boundary. An off-by-one there produces a book that is wrong in a way
nothing announces.

## How I used it in the project

Sententia Phase 5: a WAL with CRC-verified records and three sync
policies, a stable store for the election term and vote, engine
snapshots, and recovery that loads a snapshot and replays the remainder.

The most satisfying part was how little recovery code there is. It loads
a snapshot, then runs the same `engine.apply` loop the network path runs.
There is no separate reconstruction logic that could disagree with normal
operation, because there is no separate logic at all.

The stable store closed a real hole rather than adding a feature. Phase 4
kept the election term and vote in memory, so a node that voted, crashed
and restarted forgot the vote and could vote again in the same term. Two
candidates could then each collect a majority. The no-split-brain
argument assumes the vote is remembered, and memory does not survive a
crash. So term and vote are now written and flushed **before** the node
replies to a vote request, which is the same write-ahead rule applied to
a promise instead of to data.

## What surprised me

**A test can assert a property it is structurally incapable of
observing.** I wrote a test called "snapshot entries are not applied
twice", it passed, and I felt good about it. Then I deliberately broke
the boundary check with an off-by-one and the test still passed.

The reason was that my snapshot function truncated the entire log, so
there was never any overlap for the skip logic to handle. The test set up
a world where the bug could not manifest, and then confirmed it did not
manifest. It was not a weak test, it was a vacuous one, and only
deliberately breaking the code revealed that.

I now think the sabotage check is not an optional nicety. Any test
asserting that something bad does not happen should be run once against
an implementation where the bad thing does happen. It has caught
something in three phases running.

**Torn writes are more mundane than I expected and worse than I
expected.** I had thought of partial writes as an exotic failure needing
exotic hardware conditions. They are just what happens when a process
stops between two instructions. The reason they feel exotic is that
without checksums you never find out: you read the garbage, apply it, and
the corruption appears somewhere else entirely, much later.

**The in-memory mirror of the log was only correct right after loading
it.** My WAL kept a vector of records that was populated during replay
and never updated on append. So within a single session it was empty no
matter how much had been written, and a function that filtered it kept
nothing. An accessor that is correct only immediately after load is a
trap set for whoever calls it next, and the next caller was me, twenty
minutes later.

## References

- Mohan et al., *ARIES: A Transaction Recovery Method* (1992). The origin of most of this. Heavier than needed for a command log, and the redo/undo distinction is worth understanding even if you only need redo.
- Gray and Reuter, *Transaction Processing: Concepts and Techniques*, ch. 9 on logging.
- Pillai et al., *All File Systems Are Not Created Equal* (OSDI 2014). What ordering and atomicity guarantees filesystems actually provide, as opposed to what everyone assumes. This is the paper that makes you fsync directories.
- Ongaro and Ousterhout, *In Search of an Understandable Consensus Algorithm* (2014), section 7 on snapshotting in a replicated log.
- [`./determinism-and-replication.md`](./determinism-and-replication.md) - why replay works at all.
- [`./committed-and-uncommitted.md`](./committed-and-uncommitted.md) - what a leader crash may and may not lose.
