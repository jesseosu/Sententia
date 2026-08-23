# Phase 4 Completion Report: Leader Election

Checked against the must-haves, should-haves and definition of done in
`05_PHASE4_LEADER_ELECTION.md`.

## Must-haves

| Requirement | Status | Where |
|-------------|--------|-------|
| Three states with correct transitions | Done | `include/sententia/consensus/election.hpp`, `test_election` |
| Terms as a logical election clock | Done | `Election::observeTerm`, tested directly |
| Heartbeats plus randomised election timeouts | Done | `testRandomisedTimeoutsDiffer`, `testHeartbeatsKeepFollowersQuiet` |
| Majority-quorum voting, one vote per node per term | Done | `testOneVotePerTerm`, `testMajorityIsNotReachedByOneVoteInFive` |
| Leader failure detected, replacement elected in bounded time | Done | `testLeaderFailureTriggersReElection`, `election_demo` |
| **No split-brain, provably** | Done | `test_election_sim`, invariant checked continuously |
| Elected leader wired into the replication primary role | Done | `Replicator::setRole`, `apps/node/main.cpp` |

## Should-haves

| Requirement | Status | Where |
|-------------|--------|-------|
| 5-node cluster tested, not just 3 | Done | 3, 5 and 7 node clusters in `test_election_sim` |
| Election metrics: time to elect, rounds, term history | Done | `ElectionStats`, `LeadershipLog`, node shutdown summary |
| A log replay showing an election happening | Done | `scripts/election_demo.sh` |
| Network partitions: a minority cannot elect a leader | Done | `testMinorityPartitionCannotElectALeader` |

## Definition of done

> A cluster of 3+ nodes elects a leader on its own, detects leader
> failure, and elects a new leader automatically, with a test proving no
> split-brain ever occurs.

Met. `ctest` runs 25 tests, all green on GCC and Clang.

```
$ ./scripts/election_demo.sh ./build/node
node 3 elected leader for term 1
killed node 3
election demo OK
  first leader:  node 3, term 1
  after failure: node 2, term 2
  no term ever had two leaders
```

Three real processes, the leader killed with `SIGKILL`, a survivor taking
over in a strictly higher term. The higher term is the point: it is what
makes the dead leader harmless if it ever comes back.

## The split-brain proof

This is the phase's central claim and it deserves more than an assertion.

**The invariant:** no term ever has two leaders.

Not "at most one leader at any instant", which is stronger than Raft
guarantees and fails on correct behaviour. See `docs/failure-modes.md`
entry 8.

**How it is checked:** the election is a pure state machine with no
socket, no clock and no thread, so an entire cluster runs in one process
on a virtual clock over a bus that can delay, drop, partition and crash
on command. The invariant is checked after every message delivery and
every tick.

| | |
|---|---|
| Randomised fault schedules | 115 |
| Cluster sizes | 3, 5, 7 |
| Message loss rates | 0, 10, 15, 20 percent |
| Fault epochs per schedule | 60 |
| Total fault injections | roughly 6,900 |
| Assertions | 7,744 |
| Reproducible from a seed | yes, exactly |

**And the test was verified to detect what it claims to.** Two
sabotages, applied separately:

| Sabotage | Result |
|----------|--------|
| `majority()` returns 1 | `SPLIT BRAIN in term 1 (seed 4)`, and many more |
| One-vote-per-term rule removed | `SPLIT BRAIN in term 1 (seed 4)`, and many more |

Restoring either gives a clean pass. A split-brain test that cannot
detect split-brain is decoration.

## Design notes

**The election is a pure state machine.** Time arrives as a parameter,
messages arrive as function calls, and everything it wants done comes
back as a list of `Action` values for the caller to perform. Same
discipline as Phase 1 (no clock in the engine) and Phase 2 (no sockets in
the framing layer), and here it is the only reason the simulation above
is possible.

**Randomness, deliberately.** Phase 1 banned randomness from the matching
engine because replicas must agree on its output. Election requires it,
because followers must disagree about when to act, or they all time out
together and split the vote forever. These are opposite requirements of
different state machines, not a contradiction. The randomness is a seeded
LCG per node, so a run replays exactly.

**The election restriction.** A voter refuses a candidate whose log is
behind its own. Majority voting alone does not prevent a node that missed
entries from winning and silently dropping already-committed data.
Simplified relative to real Raft: sequence numbers are compared rather
than `(term, index)` pairs, which is sufficient while only one node has
ever written the log. Noted in `docs/consensus.md` rather than glossed
over.

**Heartbeats are empty `AppendEntries`,** as in Raft, so there is no
separate liveness path that could disagree with the replication path
about who leads.

**Failover resets replication state.** On becoming leader, the replicator
discards everything it believed about each backup and re-probes. That
knowledge belonged to the previous leader, and a failover is exactly what
invalidates it.

## Problems hit this phase

Two, both recorded with detail in `docs/failure-modes.md`, and both in
test or plumbing rather than protocol:

- **The first invariant was wrong** and called correct behaviour a bug.
  Understanding why is most of understanding the protocol.
- **A decoder silently dropped every reply** after a field was added to
  the encoder and not the decoder, because `AppendEntries` and
  `AppendResponse` had no round-trip codec coverage from Phase 3. There
  is now coverage for all eight message types plus a guard that fails if
  a ninth is added without it.

## Known gaps, carried forward deliberately

- **Term and vote are not durable.** A restarted node forgets who it
  voted for and can vote again in what it thinks is a fresh term, which
  breaks one-vote-per-term under crash-restart. This is a genuine
  correctness hole, not a nicety, and it is Phase 5 because the fix needs
  the same durable storage that log persistence does.
  `Election::resetVolatileState` models the loss explicitly.
- **No log divergence repair.** See the election restriction above.
- **No pre-vote.** A node returning from a partition bumps the term and
  briefly disrupts a healthy leader.
- **No leadership transfer** on clean shutdown.
- **No membership changes.** The cluster is fixed at startup.
- **A new leader does not reconcile follower logs.** It probes and
  resends, which is correct while logs cannot diverge. Once they can,
  Phase 5, it will need more.

## What this sets up

Phase 5 (fault-tolerant recovery) now has a clear and non-optional list:
durable term, durable vote, durable log, snapshots for backups that fall
behind the retained log, and per-entry terms so divergent logs can be
repaired. Each of those is already a named gap with a reason, rather than
something to discover.
