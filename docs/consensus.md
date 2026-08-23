# Leader Election

How the cluster picks a leader without being told, notices when the
leader dies, and replaces it. Raft-style, and the reasoning behind each
rule.

## What changed

Through Phase 3 the primary was designated by hand: I decided which node
led and passed `--role primary`. That works until the primary dies at
3am and somebody has to log in and promote the backup.

Now the cluster decides for itself.

## Three states and a logical clock

Every node is a **follower**, a **candidate**, or a **leader**.

```
        starts here
             |
             v
      +-------------+   election timeout   +-------------+
      |  FOLLOWER   | -------------------> |  CANDIDATE  |
      +-------------+                      +-------------+
             ^                                | |      |
             |  sees a higher term,           | |      | wins a majority
             |  or a leader of its term       | |      v
             |                                | |  +----------+
             +--------------------------------+ +->|  LEADER  |
                                                   +----------+
                     any node seeing a higher term becomes a follower
```

A **term** is a monotonically increasing integer, and it is the logical
clock of the protocol. Every election is for a new term. Terms never go
backwards, and they are how any two nodes decide whose information is
newer without consulting a wall clock they would disagree about anyway.

**The single most important rule:** any node that sees a term higher
than its own adopts that term and becomes a follower, whatever it was
doing. That one rule is what makes a stale leader harmless. A leader
that was partitioned away and comes back finds the world has moved on and
stands down, without any explicit partition detection anywhere in the
code.

## Why a majority

A candidate needs votes from more than half the cluster. For three nodes
that is two, for five it is three.

The reason is one sentence: **two majorities of the same set must
overlap in at least one member.** Combine that with "each node votes at
most once per term" and two candidates cannot both win the same term,
because the overlapping node would have had to vote twice.

That is the whole no-split-brain argument. Not a heuristic, not a race
that is unlikely to happen, an arithmetic impossibility.

It also explains why a minority partition goes quiet. Two nodes cut off
from a five-node cluster can never reach three votes no matter how long
they try, so they never elect anyone. They keep timing out and burning
through terms, which looks like thrashing and is actually correct: a
minority gives up **availability**. It does not get to give up
consistency instead.

Note that even cluster sizes buy nothing. Four nodes still need three to
agree, the same as five, so four tolerates one failure where five
tolerates two. That is why odd sizes are the convention.

## Randomised timeouts, and the contradiction that isn't

Followers start an election when they have not heard from a leader for a
while. If every follower used the same timeout they would all notice at
the same instant, all become candidates, all vote for themselves, and
split the vote. Nobody reaches a majority. They all time out again
together and do it again, forever.

Randomising the timeout breaks the symmetry. One node reliably gets there
first, and the others have usually granted it a vote before their own
timers expire. Split votes still happen, but a split is followed by fresh
random timeouts, so the chance of splitting `n` times running falls off a
cliff.

Two supporting details matter as much as the randomisation:

- **Granting a vote resets the voter's timer.** A node that votes and
  then immediately runs its own election would split the vote it just
  helped.
- **A candidate that times out starts a whole new term** with a freshly
  randomised timeout, rather than retrying the same one.

Here this looks like a contradiction with Phase 1, which banned
randomness from the matching engine because replicas must agree on its
output. It is not a contradiction, it is the opposite requirement of a
different state machine.

> The matching engine must be **deterministic** so that replicas agree.
> The election must be **randomised** so that replicas disagree about
> when to act.

Agreement on output, disagreement on timing. Both are engineered on
purpose. The randomness here is a seeded LCG owned by each node, so a
given seed replays exactly, which is what makes the simulator below
reproducible.

## The election restriction

Majority voting alone is not enough. A voter also refuses any candidate
whose log is behind its own.

Without this, a node that missed recent entries could win an election and
become the authority, and entries that were already committed elsewhere
would silently vanish. Committed data disappearing is worse than
unavailability, so the check is not optional.

`RequestVote` carries `lastLogSeq`, and a voter grants only if the
candidate is at least level with it.

**Where this is simplified.** Real Raft compares `(lastLogTerm,
lastLogIndex)` because logs can genuinely diverge: two leaders in
different terms can each have written different entries at the same
index. Sententia compares sequence numbers alone, which is sufficient
here because only one node has ever written the log and there is no
divergence to repair. Handling genuine log conflicts needs per-entry
terms and a truncate-and-overwrite path, and that arrives with durability
in Phase 5. This is a known simplification, written down rather than
glossed over.

## The pure state machine, and why it matters

`consensus::Election` owns no socket, reads no clock, and starts no
thread. Time arrives as a parameter. Messages arrive as function calls.
Everything it wants done comes back as a list of `Action` values for the
caller to perform.

This is the same discipline as Phase 1 (no clock in the engine) and
Phase 2 (no sockets in the framing layer), applied to the hardest
component in the project, and here it buys the most.

Split-brain appears only under specific interleavings of timeouts,
partitions and crashes. You will not find those by starting three
processes and pulling a cable, because you will never happen to pull it
at the microsecond that matters. But a pure state machine can be run
inside a simulator:

- a whole cluster in one process,
- on a virtual clock, so a simulated hour costs milliseconds,
- over a bus that can delay, drop, partition and crash at will,
- with the invariant checked after **every** delivery and **every** tick,
- and the entire schedule derived from one seed, so a failure replays
  exactly.

`tests/test_election_sim.cpp` runs 115 randomised fault schedules across
3, 5 and 7 node clusters at up to 20 percent message loss, 60 fault
epochs each, roughly 6,900 fault injections in total, with the invariant
checked continuously.

## The invariant, stated carefully

> **No term ever has two leaders.**

Note what is deliberately *not* claimed: "at most one leader at any
instant". That is too strong, and it would fail on correct behaviour.

When a leader is partitioned away, the majority elects a replacement in a
higher term while the old leader has no idea. For a while there really
are two nodes calling themselves leader. That is not split-brain, because
they are in **different terms**, and the isolated one cannot reach a
majority, so it cannot commit anything. When the partition heals it sees
the higher term and stands down.

Getting this distinction right is most of understanding why the protocol
works, and the first version of the minority-partition test got it wrong.
See `docs/failure-modes.md`.

The tests verify the invariant can actually fail. Setting `majority()` to
1, or removing the one-vote-per-term rule, both produce immediate
split-brain reports with named reproducible seeds. A split-brain test
that cannot detect split-brain is decoration.

## Integration with replication

The elected leader becomes the Phase 3 replication primary.

- On `BecameLeader`, the node sets the replicator's role to `Primary` and
  its term to the new term.
- On `SteppedDown`, back to `Backup`.
- Taking over **resets everything the replicator believed about each
  backup**, and re-probes. That knowledge belonged to the previous leader
  and a failover is exactly what invalidates it.
- `AppendEntries` and `AppendResponse` now carry the term. A replicator
  receiving entries from an older term rejects them, which is what stops
  a stale leader corrupting a follower that has moved on.

Heartbeats are empty `AppendEntries`, exactly as in Raft. Unifying them
means there is no separate liveness path that could disagree with the
replication path about who leads.

## Tuning

| Setting | Default | Why |
|---------|---------|-----|
| Heartbeat interval | 50 ms | Must be comfortably below the minimum election timeout, or followers unseat a healthy leader |
| Election timeout | 150 to 300 ms | Spread must exceed a typical round trip so one candidate reliably gets there first |

The usual guidance is that the timeout should be an order of magnitude
above the round-trip time and the heartbeat an order of magnitude below
the timeout. Too tight and healthy leaders get unseated; too loose and
failover is slow.

## What is deliberately not here

- **No durable term or vote.** Both are in memory, so a restarted node
  forgets who it voted for and can vote again in what it thinks is a
  fresh term. That is a real correctness hole under crash-restart, and it
  is honestly a Phase 5 item, since fixing it needs the same durable
  storage that log persistence does. `resetVolatileState` models the loss
  explicitly rather than hiding it.
- **No log divergence repair.** See the election restriction above.
- **No pre-vote.** A node returning from a partition bumps the term and
  briefly disrupts a healthy leader. Raft's pre-vote extension avoids it.
- **No leadership transfer.** A leader shutting down cleanly could hand
  over instead of letting the cluster time out.
- **No membership changes.** The cluster is fixed at startup. Joint
  consensus is a genuinely hard problem in its own right.
