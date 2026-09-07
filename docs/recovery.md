# Durability and Recovery

How a node survives being killed, and what it can and cannot promise
afterwards.

## What changed

Everything through Phase 4 lived in memory. A node that died lost its
log, and a whole-cluster restart lost the venue. Phase 4 also shipped a
real correctness hole: a node that voted, crashed, and restarted forgot
the vote, so it could vote twice in one term and let two candidates each
reach a majority. That is split-brain by another route.

Phase 5 puts three things on disk.

```
<data-dir>/state       term and vote, written before any vote is cast
<data-dir>/log.wal     the command log, written before any commit
<data-dir>/snapshot    engine state as of some sequence number
```

## The write-ahead rule

A command reaches disk and is flushed **before** it is treated as
committed. Not after, not alongside.

The ordering is the whole guarantee. If the process dies at any instant,
everything a client was told succeeded is on disk, and anything not on
disk was never acknowledged. Getting it backwards means telling a client
a trade happened and then forgetting it, which is the single worst thing
a venue can do.

## Torn writes

A crash mid-write leaves a partial record. The file is not corrupt in an
obvious way; it simply ends in a record that is half there, and reading
it back naively gives garbage that looks like data.

Every record carries its payload length and a CRC32:

```
 offset  size  field
 0       4     magic    0x4C41575F, "_WAL"
 4       4     seq
 8       4     length
 12      4     crc32    of the payload only
 16      ...   payload
```

On open, records are replayed until one fails to verify, and the file is
truncated there. A half-written record at the tail is **normal** after a
crash, not an error: it is a command that was never acknowledged, so
discarding it is correct.

Everything after a bad record is discarded too, not just the bad one.
Once a record fails to verify there is no way to trust what follows it.

## The durability tradeoff

| Policy | fsyncs per 100 appends | On power loss |
|--------|------------------------|---------------|
| `EveryWrite` | 100 | Nothing acknowledged is lost |
| `Batched` (N=10) | 10 | Up to 9 acknowledged commands lost |
| `Never` | 0 | Survives a process crash, not a power cut |

Measured directly in `test_durability`.

This is the same shape of decision as synchronous versus asynchronous
replication in Phase 3: how much do you pay, in the critical path, for
how strong a promise. `Batched` is what most production systems actually
run, with N chosen against how much loss the business can tolerate.

`Never` is for tests and benchmarks. It is never right for a venue, and
naming it honestly is better than hiding it behind a flag called "fast".

## Atomic file replacement

The stable store and the snapshot are written whole, so they use a
different trick: write to a temporary file, fsync it, rename it over the
target, then **fsync the containing directory**.

Rename within a directory is atomic on POSIX, so a crash leaves either
the complete old file or the complete new one, never a mixture. Writing
in place would leave exactly the torn state this is avoiding.

The directory fsync is the part that is easy to miss. The rename is
atomic, but it is only *durable* once the directory entry itself reaches
disk. Skipping it produces a bug that looks fine in every test and loses
the rename on a real power cut.

## Recovery

```
1. Load the snapshot, if there is one.        engine is now at seq S
2. Replay WAL entries with seq > S.           engine is now at the head
3. Done.
```

Recovery is **not a special code path**. It is the ordinary apply loop,
fed from disk instead of from a socket. That works only because the
engine is deterministic, which is Phase 1 paying off for the fourth time:
same commands, same order, same state.

`test_recovery` asserts the recovered state checksum equals the original
exactly, not approximately, and that recovering five times from the same
files gives the same answer every time.

## Snapshots

Replaying the whole log works and gets slower every day the venue runs. A
log with fifty million commands means fifty million commands to replay
before the node is useful.

A snapshot is the engine as of some sequence number. Recovery then starts
there and replays only what came after, so recovery time stops being a
function of total history and becomes a function of time since the last
snapshot, which is a number you control.

Snapshotting a matching engine means capturing more than the visible
book. `EngineSnapshot` also carries `arrivalCounter`, and leaving it out
would restore a book that looks correct and quietly breaks time priority
for every order placed afterwards. That is the kind of bug that surfaces
weeks later as unfair fills.

A snapshot records the state checksum it was taken at. On load, the
restored engine's checksum is recomputed and compared, and a mismatch is
refused. A snapshot that silently restores a subtly wrong book is far
worse than one that fails to load.

### The gap between snapshot and truncation

`takeSnapshot` writes the snapshot first and truncates the log second.
That order is deliberate: the reverse loses data if the process dies
between the two, because the log would be gone and the snapshot would not
exist yet.

The cost of that ordering is that a crash **in the gap** leaves a
snapshot covering a prefix and a log that still contains that same
prefix. Recovery then sees every entry twice over and must apply exactly
the ones after the boundary. An off-by-one there produces a book that is
quietly wrong.

`testCrashBetweenSnapshotAndTruncationDoesNotDoubleApply` constructs
exactly that on-disk state. An earlier version of the test snapshotted at
the log head, which left no overlap at all, and a deliberate off-by-one
went completely unnoticed. See `docs/failure-modes.md`.

## Per-entry terms and log matching

Phase 3 matched log positions on the sequence number alone, which was
enough while only one node had ever written the log. Once leaders can
change, two leaders in different terms can each have written a
**different** entry at the same sequence number. Matching on the number
alone would splice two histories together and produce a book that never
existed on any node.

So every entry carries the term of the leader that created it, and
`AppendEntries` carries `prevTerm` alongside `prevSeq`. A follower
accepts entries only if it holds an entry at `prevSeq` with exactly that
term. On a mismatch it rejects, the leader backs up, and the follower
truncates its divergent tail.

## The commit rule, and why truncation is safe

The subtlest correctness property in the project.

Truncating a follower's log sounds alarming: entries are being deleted.
It is safe because of what "committed" means.

**An entry is committed once a majority holds it.** Combined with the
election restriction from Phase 4 (a candidate whose log is behind cannot
win), any node that can become leader already has every committed entry.
So anything a follower truncates was, by construction, never committed,
and no client was ever told it succeeded.

The code asserts this rather than assuming it: truncating an entry at or
below the commit index refuses and logs loudly, because reaching that
state would mean the safety argument had failed somewhere.

### Majority, not unanimity

Phase 3 committed when **every** backup held an entry. Simpler, and it
means one slow or dead backup stalls the cluster, which is the opposite
of fault tolerance. Phase 5 uses a majority, so the cluster keeps working
while a minority is down.

A consequence worth stating plainly: **a two-node cluster tolerates zero
failures**, because a majority of two is two. It is no more available
than a single node, and it is why real deployments use odd sizes. Three
tolerates one, five tolerates two.

### The Figure 8 rule

A leader may **not** commit an entry from a previous term just because a
majority now holds it.

Raft's paper has a five-node scenario where doing so lets an entry that
was reported committed be overwritten later. The reason is that a
majority holding an old entry does not make it safe, because a future
leader could still be elected without it. Only once the current leader
commits an entry from its **own** term does the election restriction
guarantee that every future leader carries everything up to that point,
and committing a current-term entry carries the earlier ones with it.

So `advanceCommitIndex` refuses to advance unless the entry at the
candidate commit point was written in the current term. It is four lines
and it is the difference between correct and subtly, rarely wrong.

## What a leader crash actually loses

Nothing committed. Possibly some uncommitted entries, which is correct
because nobody was told about them.

`testLeaderCrashLosesNothingCommitted` builds a leader whose log runs
ahead of its commit point, kills it, recovers from disk, and asserts that
replaying only the committed prefix reproduces exactly the state both
nodes agreed on beforehand.

## Known gaps

- **InstallSnapshot is not a wire message yet.** `DurableState::installSnapshot`
  exists and is tested, but a leader does not yet push a snapshot to a
  lagging follower over the network; the plumbing is in place and the
  transport path is not.
- **Snapshots are not automatic.** Nothing takes one on a timer or a size
  threshold, so an operator has to drive it.
- **No log index for random access.** `CommandLog::at` is O(1) on the
  in-memory deque, but the WAL is replayed linearly at startup with no
  index, so opening a very large log is O(size).
- **The WAL keeps every record in memory.** Fine at the scale tested,
  wrong for a log larger than RAM. Bounded in practice by snapshotting.
- **No checksum over the whole WAL file**, only per record. A record that
  verifies individually inside a corrupted file is still trusted.
