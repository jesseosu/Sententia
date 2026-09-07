# Phase 5 Completion Report: Fault-Tolerant Recovery

Checked against the must-haves, should-haves and definition of done in
`06_PHASE5_RECOVERY.md`.

## Must-haves

| Requirement | Status | Where |
|-------------|--------|-------|
| Write-ahead log; commands persisted before commit | Done | `src/storage/wal.cpp`, `DurableState::appendEntry` |
| A node can restart and reconstruct correct state by replaying | Done | `testRestartReplaysToTheSameState` |
| Snapshotting to bound recovery time | Done | `src/storage/snapshot.cpp`, `testSnapshotBoundsRecovery` |
| Rejoin via snapshot plus recent commands, no missed or duplicated entries | Partial | `testFollowerTooFarBehindInstallsASnapshot`, `testCrashBetweenSnapshotAndTruncationDoesNotDoubleApply`. The wire message is not plumbed; see gaps. |
| Follower-crash and leader-crash both recover correctly | Done | `testLeaderCrashLosesNothingCommitted`, `test_recovery` |
| Commit rule guarantees a new leader has all committed commands | Done | majority commit plus the Figure 8 rule in `advanceCommitIndex` |
| Recovery verification tests pass, including kill-leader-mid-stream | Done | `test_recovery`, 5,211 assertions |

## Should-haves

| Requirement | Status | Where |
|-------------|--------|-------|
| Configurable durability with a measured difference | Done | three `SyncPolicy` values, measured in `testBatchedSyncsLessOften` |
| Adding a brand new node to a running cluster | Done | `testFollowerTooFarBehindInstallsASnapshot` starts from nothing |
| Chaos harness killing and restarting under load | Partial | Phase 4's simulator covers crash and restart for election; durability chaos is not wired to it |
| Recovery-time metrics | Done | `RecoveryReport` reports entries replayed with and without a snapshot |

## Definition of done

> Any node can crash at any point, restart, rejoin, and the cluster
> converges to a single consistent state with no lost committed orders
> and no duplicates.

Met for the properties that can be asserted, with one honest caveat below
about snapshot transfer over the wire.

`ctest` runs 27 tests, all green on GCC and Clang, Release and Debug.

## What is now on disk

```
<data-dir>/state       term and vote, written before any vote is cast
<data-dir>/log.wal     the command log, written before any commit
<data-dir>/snapshot    engine state as of some sequence number
```

The stable store closes a real correctness hole rather than adding a
feature. Phase 4 kept the term and vote in memory, so a node that voted,
crashed and restarted forgot the vote and could vote again in the same
term, letting two candidates each reach a majority. The no-split-brain
argument assumes the vote is remembered.

## The commit rule changed, and it matters

Phase 3 committed when **every** backup held an entry. Phase 5 commits on
a **majority**, plus the Figure 8 restriction that a leader may only
advance the commit point to an entry from its own term.

Two consequences worth stating plainly:

**The cluster now makes progress with a minority down.** That is the
entire point of tolerating failure, and the old rule did not.

**A two-node cluster tolerates zero failures**, because a majority of two
is two. It is no more available than a single node. `test_replication`
now asserts this directly: with the backup dead, the leader keeps
accepting into its log and applies nothing, because nothing can commit.
That test previously asserted the opposite, and the old expectation was
the unsafe one.

## Durability tradeoff, measured

| Policy | fsyncs per 100 appends | A power cut loses |
|--------|------------------------|-------------------|
| `EveryWrite` | 100 | nothing acknowledged |
| `Batched` (N=10) | 10 | up to 9 acknowledged commands |
| `Never` | 0 | everything not yet flushed |

## Problems hit this phase

Six, all catalogued with detail in `docs/failure-modes.md`. Two are worth
pulling out here.

**A test that could not fail.** `testSnapshotEntriesAreNotAppliedTwice`
passed, and kept passing when the boundary check was deliberately broken
with an off-by-one. The snapshot function truncated the whole log, so no
overlap existed and the skip logic never ran. The test had constructed a
world where the bug could not occur, then confirmed it did not occur. Both
the code and the test were wrong: truncation is now partial, and the test
now builds the realistic overlap (a crash between writing the snapshot
and truncating the log) and asserts exactly 500 entries replayed, not 501.

**Two correct rules that deadlocked.** Tracking each follower's applied
index as its match point hung the entire suite with no error. A follower
applies only what the leader says is committed; the leader cannot commit
until followers report holding. The match point must be the follower's
log head, because holding an entry and having applied it are different
facts.

## Testing notes

Every durability and recovery test was verified to fail against a
deliberately broken implementation:

| Sabotage | Caught by |
|----------|-----------|
| Trust records whose CRC does not match | `test_durability`, 2 failures |
| Off-by-one at the snapshot boundary | `test_recovery`, 3 failures |

The off-by-one initially slipped through, which is how the vacuous test
above was found.

## Known gaps, carried forward deliberately

- **InstallSnapshot is not a wire message.** `DurableState::installSnapshot`
  works and is tested, but a leader does not push a snapshot to a lagging
  follower over the network. A follower behind the retained log is
  detected and reported rather than repaired automatically.
- **Snapshots are not automatic.** Nothing takes one on a timer or size
  threshold; an operator drives it.
- **The WAL is replayed linearly with no index**, so opening a very large
  log is O(size), and it keeps every record in memory. Bounded in
  practice by snapshotting, wrong for a log larger than RAM.
- **No whole-file checksum on the WAL**, only per record.
- **Durability chaos is not wired into the Phase 4 simulator.** Crash and
  restart are covered there for election, and durability is covered
  separately; the combination is not randomised.
- **No membership changes**, still. The cluster is fixed at startup.

## What this sets up

Phase 6 is benchmarking and hardening, and it now has real things to
measure: fsync cost per policy, recovery time with and without snapshots,
commit latency under a majority rule versus the old unanimous one, and
the throughput cost of durability in the critical path.
