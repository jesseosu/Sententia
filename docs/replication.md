# Replication

How a primary keeps a backup's order book identical to its own, and the
reasoning behind each decision.

## Ship commands, not state

The first decision, and the one everything else follows from.

A backup needs the same order book as the primary. There are two ways:

**Ship the state.** After every command, send the resulting book. Simple,
obviously correct, and unusable. A single order is a few dozen bytes; a
book with eighty thousand resting orders is megabytes. The cost per
command would scale with the *size of the book* rather than the size of
the change, so replication would be most expensive exactly when the venue
is busiest. Shipping diffs instead trades that for a diff format and
still sends consequences rather than causes.

**Ship the commands.** Send the backup the same commands, in the same
order, and let it compute the book itself. The message stays a few dozen
bytes no matter how deep the book gets.

Measured here: **72.6 bytes per command**, flat, regardless of book depth.

The second only works if both nodes compute the same state from the same
input. That is exactly the determinism contract Phase 1 established, and
it is why the determinism test is the gate on this phase. This is
state-machine replication, the idea underneath Raft, Paxos, and every
database that replicates by shipping its write-ahead log.

Confirmation that it holds end to end: replaying `scripts/sample_orders.txt`
through the single-process driver and through a replicated primary/backup
pair produces the same state checksum, `17373410596180621386`, on all
three.

## Order matters as much as content

Phase 1's sensitivity test showed that swapping two adjacent commands
changes the engine's output. So the *order* of the log is as much a part
of the input as the commands in it.

That is why the primary assigns sequence numbers, and why Phase 4 needs
an election. With no agreed primary there is no agreed order, and with no
agreed order there is no agreed state. Leader election is not a
reliability feature bolted on later; it is what makes the ordering
well-defined once more than one node can accept work.

## Sequence numbers

The primary assigns every command a monotonic, gapless sequence starting
at 1. Zero means "nothing yet", which is why numbering does not start
there.

These are distinct from the engine's own internal command counter. The
engine's counter is an implementation detail; the log's sequence is what
two nodes agree on.

The backup applies entries only when `prevSeq` matches what it has
already applied. If it does not, it has a gap, and it says so rather than
applying out of order. Applying out of order would break determinism
silently and unrecoverably, so it is refused at both layers:
`CommandLog::appendAt` accepts nothing but exactly `lastSeq() + 1`.

## The acknowledgement decision

The core consistency tradeoff, implemented both ways because the
difference is worth being able to measure.

### Synchronous

The primary applies a command only once a backup acknowledges holding it.
A client told "done" means both nodes have it, so a primary crash loses
nothing.

The cost is a network round trip in the critical path, and the primary
can go no faster than its slowest backup.

With no backup connected, `submit` returns `NoBackup` rather than
applying anyway. Applying would silently downgrade to asynchronous and
quietly drop the guarantee the caller asked for.

### Asynchronous

The primary applies immediately and replicates in the background. Faster,
and the backup may lag. A primary crash can lose whatever had not reached
the backup.

### The numbers

20,000 commands, two nodes over loopback, GCC 13.3 Release, in a shared
cloud container.

| Mode | Window | Throughput | Submit p50 | Submit p99 |
|------|--------|-----------|-----------|-----------|
| Synchronous | 1 | 8,183 cmd/s | 61,325 ns | 119,946 ns |
| Synchronous | 8 | 8,264 cmd/s | 2,447 ns | 5,966 ns |
| Synchronous | 64 | 8,277 cmd/s | 2,432 ns | 5,682 ns |
| Asynchronous | n/a | 15,692 cmd/s | 2,787 ns | 4,822 ns |

Two things worth reading off this table.

**Asynchronous is about 1.9x the throughput of synchronous.** That is the
price of the guarantee, and it is a real price, not a rounding error.

**The window is what hides the round trip from the caller.** At window 1,
strict lockstep, submit latency is 61 microseconds: the caller waits for
the backup on every single command. At window 8 it drops to 2.4
microseconds, a 25x improvement, because the primary can have eight
commands outstanding and the caller almost never waits. Throughput barely
moves, because it was never limited by the caller.

That is the more interesting result. Pipelining does not weaken the
guarantee at all: a command is still only applied once acknowledged. It
just stops the guarantee being paid for in *caller-visible latency*. What
it does cost is crash exposure: with a window of 8, up to 8 commands may
be in flight and unacknowledged when a primary dies, versus 1.

So the honest framing is not "synchronous versus asynchronous" but a dial
with three positions: how many commands may be in flight, how much
latency the caller sees, and how much a crash can lose. Asynchronous is
that dial turned to infinity.

## Backpressure

Replication is where a peer can fall behind, so it is where flow control
stops being optional. Phase 2's transport had no cap on its outbound
queue at all, which was invisible while the only traffic was heartbeats
and would have been a memory leak here. See `docs/failure-modes.md`.

Backpressure now surfaces all the way up:

1. `FrameWriter` has a high-water mark, 8 MiB by default.
2. `Transport::send` returns `WouldOverflow` rather than queueing past it.
3. `Replicator::submit` returns `Busy` when a backup is saturated, or
   when the synchronous window is full.
4. The caller retries.

The client seeing `Busy` is the honest outcome. The alternative is the
primary absorbing the backlog in memory until it dies, which converts a
slow backup into a failed primary.

## Catch-up

A backup that reconnects may have applied anything from nothing to
everything, and the primary cannot know which. So it probes: an empty
`AppendEntries` whose `prevSeq` is the primary's head. The backup either
agrees, or reports where it actually got to, and the primary resends from
there in batches of up to 512 entries.

Batching is capped so a backup a million entries behind gets caught up
over many messages rather than monopolising the connection with one
enormous one.

The correctness of catch-up was established in Phase 1, three phases
before anything needed it: the prefix-replay test proved that applying a
prefix and then continuing produces the same state as applying everything
at once. Catch-up is that property used in anger.

## Divergence detection

Every `AppendResponse` carries the backup's state checksum. When the two
nodes have applied to the same sequence, their checksums must match, and
a mismatch is logged loudly.

This is why `OrderBook::checksum` walks the book in canonical order and
is a function of *state* rather than of history, asserted directly in
`test_order_book`. It means two nodes that reached the same place by
different routes, one by live replication and one by catch-up replay,
compare equal. Comparing a single integer instead of shipping a book is
the entire reason the check is cheap enough to run on every response.

## Log compaction

The log grows for as long as the process runs unless something trims it.
`Replicator::compactLog` drops entries that every backup has acknowledged
*and* the primary has applied. Anything else might still be needed for a
catch-up.

`CommandLog::canServeFrom` reports honestly when a backup needs entries
that have been truncated away. Phase 3 has no snapshot mechanism, so the
replicator logs the condition rather than sending a gap and corrupting
the backup. That is a real gap in the design, recorded rather than
papered over, and it is Phase 5 work.

Note that compaction is not automatic. Nothing calls it on a timer, so a
long-running primary grows its log until the application decides to trim.

## What is deliberately not here

- **No failover.** A backup never becomes a primary. Phase 4.
- **No durability.** The log is in memory; a primary that dies loses it.
  Phase 5.
- **No snapshots.** As above.
- **No quorum.** One primary, one or more backups, and `commitSeq` is the
  minimum across all of them, so every backup must acknowledge. That is
  stricter than a majority quorum and simpler to reason about, and it
  means one stuck backup stalls synchronous progress. Quorums arrive with
  elections in Phase 4, because both need the same notion of a majority.
