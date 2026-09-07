# Phase 3 Completion Report: State Replication

Checked against the must-haves, should-haves and definition of done in
`04_PHASE3_REPLICATION.md`.

## Must-haves

| Requirement | Status | Where |
|-------------|--------|-------|
| State-machine replication with monotonic sequence numbers | Done | `src/replication/command_log.cpp`, `replicator.cpp`, `docs/replication.md` |
| Backup applies in exact primary order; detects gaps | Done | `Replicator::handleAppendEntries`, `CommandLog::appendAt`, `testGapDetection` |
| A defined acknowledgement model, tradeoff documented | Done | Both modes implemented, `docs/replication.md` |
| Catch-up: a lagging backup requests and applies missed commands | Done | probe plus resend, `testCatchUpAfterDisconnect` |
| Consistency test: backup state provably identical to primary | Done | `test_replication`, `test_replication_chaos` |

## Should-haves

| Requirement | Status | Where |
|-------------|--------|-------|
| Both sync and async, switchable, with a latency benchmark | Done | `bench/replication_bench.cpp` |
| Chaos test: drop the backup connection, assert convergence | Done | `tests/test_replication_chaos.cpp` |
| Metrics: replication lag, commands/sec, ack latency | Done | `ReplicationStats`, `TransportStats`, benchmark output |

## Definition of done

> A primary and a backup, where every order processed by the primary is
> replicated to the backup, and the backup's state is provably identical
> after any command sequence, including after the backup briefly
> disconnects and catches up.

Met. `ctest` runs 22 tests, all green on GCC and Clang.

The strongest single piece of evidence: replaying `scripts/sample_orders.txt`
through the Phase 1 single-process driver, through a synchronous
primary/backup pair, and through an asynchronous one all produce the same
state checksum, `17441047841503333197`.

```
$ ./scripts/replication_demo.sh ./build/node \
    scripts/sample_orders.txt sync
replication demo OK (mode=sync)
  commands applied on both nodes: 12
  state checksum on both nodes:   17441047841503333197
```

## Benchmark

20,000 commands, two nodes over loopback, Release, GCC 13.3, shared cloud
container.

| Mode | Window | Throughput | Submit p50 | Submit p99 | Bytes/cmd |
|------|--------|-----------|-----------|-----------|-----------|
| Synchronous | 1 | 8,183 cmd/s | 61,325 ns | 119,946 ns | 72.6 |
| Synchronous | 8 | 8,264 cmd/s | 2,447 ns | 5,966 ns | 72.6 |
| Synchronous | 64 | 8,277 cmd/s | 2,432 ns | 5,682 ns | 72.6 |
| Asynchronous | n/a | 15,692 cmd/s | 2,787 ns | 4,822 ns | 72.6 |

Asynchronous is about 1.9x synchronous throughput. More interesting is
the window: at strict lockstep the caller waits 61 microseconds per
command, and at a window of 8 that drops 25x while throughput barely
moves. Pipelining does not weaken the guarantee, it stops the guarantee
being paid for in caller-visible latency. What it costs is crash
exposure: more commands in flight when a primary dies.

Bytes per command is flat at 72.6 regardless of book depth, which is the
whole argument for shipping commands rather than state.

## The bug worth reading about

A use-after-move left the "how far have I sent" cursor permanently at
zero, so every submit re-sent the entire outstanding range. Replicating
1,000 commands cost **883,003 messages and 91 MB** instead of 1,001
messages and 72 KB.

**Every correctness test passed.** Re-sending changes no state: the backup
detected the overlap, applied what was new, and converged to exactly the
right checksum. The chaos test passed. The system was provably correct
and completely unusable, and only the benchmark could tell the
difference.

That is the sharpest lesson of the phase. A suite that only checks
behaviour cannot distinguish a good implementation from a catastrophic
one, as long as the catastrophic one is correct. There is now a
`testNoMessageAmplification` that asserts entries, messages and bytes
stay proportional to the command count, and the benchmark prints
`entries/cmd`, `msgs/cmd` and `bytes/cmd` next to throughput so
amplification appears in the headline rather than buried.

Full catalogue, with the Phase 2 defects too, in `docs/failure-modes.md`.

## Testing notes

**The correctness tests were verified to fail on a broken replica.**
Sabotaging the backup to skip one command in every 500 produced six
failures across every convergence assertion, with the checksums visibly
differing. Restoring it produced a clean pass. A convergence test that
cannot detect divergence is worthless, so this was checked rather than
assumed.

**Chaos is seeded.** `test_replication_chaos` severs the link at
unpredictable points across four seeds and four drop rates, from
occasional to brutal, and asserts convergence to an identical checksum
every time. The randomness comes from a seeded LCG, so a failure is
replayable. Chaos that cannot be reproduced is an anecdote, not a test.

It also asserts `gapsDetected > 0`, so the recovery path is known to have
been exercised rather than accidentally avoided.

## Test inventory added this phase

| Test | Validates |
|------|-----------|
| `test_backpressure` | Bounded queue, refusal reported, recovery when the peer resumes |
| `test_command_log` | Sequencing, gap refusal, range serving, truncation, compaction windows |
| `test_replication` | Async and sync convergence, commit semantics, sync-without-backup, gap detection, message amplification, full catch-up from zero, compaction |
| `test_replication_chaos` | Convergence under repeated link failure, 4 seeds x 4 drop rates |
| `replication_demo_sync` / `_async` | Two real processes, identical checksums, both modes |

## Known gaps, carried forward deliberately

- **No failover.** A backup never becomes a primary. Phase 4.
- **No durability.** The log is in memory. A primary that dies loses it,
  and a restarted node starts empty. Phase 5.
- **No snapshots.** A backup behind the retained log cannot catch up by
  replay. `CommandLog::canServeFrom` detects it and the replicator says
  so plainly rather than sending a gap. Phase 5.
- **Compaction is not automatic.** Nothing calls `compactLog` on a timer,
  so a long-running primary grows its log until the application trims it.
- **Every backup must acknowledge**, since `commitSeq` is the minimum
  across all of them. Stricter than a majority quorum and simpler to
  reason about, but one stuck backup stalls synchronous progress.
  Quorums arrive with elections in Phase 4, because both need the same
  notion of a majority.
- **Order ids are client-assigned and not validated across nodes.** A
  primary that accepted conflicting ids would replicate the conflict
  faithfully. The engine rejects duplicates among live orders, which is
  the only guarantee claimed.

## What this sets up

- **Phase 4 (leader election)** needs exactly what this phase made
  visible: with no agreed primary there is no agreed order, and the
  sensitivity test proved order changes outcomes. `RequestVote` and
  `VoteResponse` already have reserved slots in the message enum, and
  `commitSeq` already generalises across multiple backups.
- **Phase 5 (recovery)** needs durability and snapshots, and already has
  the replay semantics they depend on: the Phase 1 prefix-replay test
  established that applying a prefix then continuing equals applying
  everything, which is precisely what a recovering node does.
