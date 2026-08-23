# Raft Leader Election

> Two majorities of the same group must share at least one member. That member gets one vote. So two candidates cannot both win the same election. Everything else in leader election is machinery around that one sentence.

**Learned during:** Sententia Phase 4, leader election
**Tags:** `distributed-systems` `consensus` `raft` `leader-election` `split-brain` `quorum`

---

## The problem it solves

By the end of Phase 3 I had a primary that replicated to a backup, and I
picked which node was the primary by passing a command line flag. That is
fine right up until the primary dies at 3am and somebody has to wake up,
log in, and promote the backup by hand.

So the cluster needs to pick a leader itself, notice when that leader
dies, and pick another one. No human involved.

The obvious approaches all fail in instructive ways.

**Lowest id wins.** Simple, and broken: if the lowest id node is merely
slow rather than dead, half the cluster thinks it leads and half has
moved on.

**Whoever claims it first.** Two nodes claim simultaneously and both
believe they won.

**Ask a coordinator.** Now the coordinator is a single point of failure,
and you need to elect it.

The failure they all share is **split-brain**: two nodes both believing
they lead, both accepting orders, both producing an authoritative book.
For a matching engine that means two different sets of trades, both
"real", and no way to reconcile them afterwards. It is the catastrophe
this whole phase exists to prevent.

## The core idea

Three states, and a number.

A node is a **follower** (the default, passive), a **candidate** (asking
to be leader), or a **leader**.

The number is the **term**: a counter that only ever goes up. Every
election is for a new term. Terms are the logical clock of the protocol,
and the reason one is needed at all is that wall clocks on different
machines disagree, so "who is more recent" cannot be answered with a
timestamp. It can be answered with a counter everyone agrees to increment
the same way.

The mechanism, in five lines:

1. A leader sends periodic heartbeats.
2. A follower that has not heard one for a while becomes a candidate,
   increments the term, votes for itself, and asks everyone else.
3. A node votes for at most one candidate per term.
4. A candidate with votes from a **majority** becomes leader.
5. Any node that sees a term higher than its own becomes a follower.

That is it. The rest is understanding why those five rules are enough.

## How it works

```
        starts here
             |
             v
      +-------------+   no heartbeat       +-------------+
      |  FOLLOWER   | -------------------> |  CANDIDATE  |
      +-------------+   for a while        +-------------+
             ^                                | |      |
             |  sees a higher term,           | |      | majority
             |  or a leader of its own term   | |      v
             |                                | |  +----------+
             +--------------------------------+ +->|  LEADER  |
                                                   +----------+
```

Rule 5 is doing far more work than it looks like. It is the only thing
needed to handle a leader that gets partitioned away and comes back. That
node still thinks it leads. It sends a heartbeat, a follower replies with
a higher term, and the old leader steps down immediately. There is no
partition detection code anywhere. The term handles it.

## The key insight

**Two majorities of the same set must overlap.**

If a group has five members, any two subsets of size three share at least
one member. Combine that with "one vote per node per term" and two
candidates cannot both win the same term, because the overlapping node
would have had to vote twice.

That is not a heuristic or a race that is unlikely to matter. It is
arithmetic. Split-brain in a single term is impossible, not improbable.

This also explains, for free, what happens to a minority partition. Two
nodes cut off from a cluster of five can never assemble three votes, no
matter how long they try. So they never elect anyone. They keep timing
out, keep incrementing terms, and keep failing. That looks like thrashing
and is exactly right: **a minority gives up availability. It does not get
to give up consistency instead.** The same quorum rule that prevents
split-brain is what makes the minority go quiet.

The second insight is subtler and it is the one I would want to be asked
about.

**Randomised election timeouts are load-bearing, not a tuning detail.**

If every follower used the same timeout they would all notice the dead
leader at the same instant, all become candidates, all vote for
themselves, and split the vote. Nobody gets a majority. They all time out
again together and do it again. Forever. A perfectly correct
implementation that never elects anybody.

Randomising breaks the symmetry. One node usually gets there first and
the others have already granted it a vote before their own timers fire.
Splits still happen, but each retry re-randomises, so the probability of
splitting n times in a row collapses.

Two small rules hold it up:

- **Granting a vote resets the voter's timer.** Otherwise a node votes
  and then immediately runs its own election, splitting the vote it just
  helped.
- **A timed-out candidate starts a whole new term**, with a fresh random
  timeout, rather than retrying the old one.

And the third insight, which only appeared once I had it working:

**Majority voting alone can still lose committed data.** A node that
missed recent entries can win an election on votes alone and become the
authority, and anything committed that it never saw quietly disappears.
So a voter also refuses any candidate whose log is behind its own. Raft
calls this the election restriction. Committed data vanishing is worse
than being unavailable, so this check is not optional, and it is easy to
miss because the system looks like it works without it.

## The tradeoffs

**Availability is capped by the quorum.** A cluster of three survives one
failure. Five survives two. You cannot have both "tolerates more
failures" and "needs fewer nodes to agree", because they are the same
dial pointed in opposite directions.

**Even cluster sizes are wasted machines.** Four nodes still need three
to agree, exactly like five, so four tolerates one failure where five
tolerates two. Odd sizes are the convention for that reason.

**Failover is not instant.** It takes an election timeout to notice the
leader is gone, plus a round of voting. Shortening the timeout makes
failover faster and makes spurious elections more likely, because a
briefly slow leader gets unseated. There is no setting that gives both.

**Every write goes through one node.** The leader is a throughput
ceiling by construction. Consensus buys agreement, not scale.

**Terms must survive a crash to be fully correct.** A node that restarts
and forgets who it voted for can vote again in what it believes is a
fresh term, which can break the one-vote-per-term guarantee. Real Raft
writes the term and vote to disk before responding. Mine does not yet,
and that is a real hole rather than a detail.

## How I used it in the project

Sententia Phase 4: a `consensus::Election` state machine, wired so the
elected leader becomes the Phase 3 replication primary.

The design decision that mattered most was making the election a **pure
state machine**. It owns no socket, reads no clock, and starts no thread.
Time arrives as a function parameter. Messages arrive as function calls.
Everything it wants done comes back as a list of actions for the caller
to perform.

This is the same discipline as keeping the matching engine free of clocks
in Phase 1 and the framing layer free of sockets in Phase 2, and here it
pays the most, because it makes split-brain **testable**. The whole
cluster runs in one process on a virtual clock, over a message bus that
can delay, drop, partition and crash on command, with the invariant
checked after every single delivery. 115 randomised fault schedules
across 3, 5 and 7 node clusters at up to 20 percent message loss, about
6,900 fault injections, and every schedule replays exactly from its seed.

I also checked the test could fail. Setting the majority to 1, and
separately removing the one-vote-per-term rule, both produced immediate
split-brain reports naming the exact seeds. A split-brain test that
cannot detect split-brain is decoration.

Heartbeats are empty `AppendEntries`, as in Raft, so there is no separate
liveness path that could disagree with the replication path about who
leads.

## What surprised me

**The invariant I first wrote down was wrong, and being wrong about it
taught me the protocol.**

My instinct was "at most one leader at a time". That is too strong, and
it fails on completely correct behaviour. When a leader is partitioned
away, the majority elects a replacement while the old leader has no idea
anything happened. For a while there genuinely are two nodes calling
themselves leader.

That is not split-brain. They are in different terms, and the old one
cannot reach a majority, so it cannot commit anything. It is a leader in
name with no power. The correct invariant is **no term ever has two
leaders**, and the difference between those two statements is most of
what makes the protocol work.

My first minority-partition test failed for exactly this reason: sometimes
the pre-partition leader landed in the minority and stayed leader in its
old term. I had written a test that called correct behaviour a bug.

**The randomness felt like it contradicted Phase 1, and thinking about
why it does not was clarifying.** Phase 1 banned randomness from the
matching engine, because replicas have to agree on its output. Phase 4
requires randomness, because followers have to disagree about when to act.

They are not in tension. They are opposite requirements of two different
state machines that happen to live in the same process. Agreement on
output, disagreement on timing, both engineered deliberately. Once I saw
it that way the "no randomness in the core" rule stopped feeling like a
principle I was violating and started looking like what it always was: a
statement about the matching engine specifically.

**Terms do more work than any other single idea here.** Stale leader
returning after a partition, late vote replies from an old election
arriving during a new one, a follower that has moved on ignoring an
outdated leader: all of it is one comparison against a counter. I kept
looking for the code that detects partitions and there isn't any. There
does not need to be.

## References

- Ongaro and Ousterhout, *In Search of an Understandable Consensus Algorithm* (2014). Section 5.2 is election, 5.4.1 is the election restriction. Genuinely readable, which was the paper's stated goal.
- https://raft.github.io - the visualisation. Watching a split vote happen and then resolve on the retry made randomised timeouts click faster than the text did.
- Ongaro's thesis, *Consensus: Bridging Theory and Practice* (2014), for pre-vote, leadership transfer, and membership changes, all of which I skipped.
- Howard and Mortier, *Paxos vs Raft* (2020), on how similar they actually are once you look past the presentation.
- [`./state-machine-replication.md`](./state-machine-replication.md) - the Phase 3 entry. Election exists because replication needs an agreed order, and an agreed order needs an agreed orderer.
- [`./determinism-and-replication.md`](./determinism-and-replication.md) - the Phase 1 entry, and the other half of the determinism story here.
