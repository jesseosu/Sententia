# Committed and Uncommitted: What a Leader Crash Can Lose

> A leader dies holding orders nobody else has seen. Which ones survive? The answer is not "the ones it wrote down". It is "the ones a majority wrote down", and the gap between those two sentences is where distributed systems get their reputation.

**Learned during:** Sententia Phase 5, fault-tolerant recovery
**Tags:** `distributed-systems` `consensus` `raft` `consistency` `fault-tolerance`

---

## The problem it solves

A leader accepts an order, writes it to its own disk, and dies. Did that
order happen?

It is a genuinely awkward question because both answers are defensible.
The order is durably recorded on a machine that will come back. But no
other node has ever heard of it, and the cluster is about to elect a new
leader that will carry on without it.

Worse, the same question has a different answer depending on when exactly
the leader died, and "when exactly" is not something you get to control.

Get this wrong in the direction of being too generous and you have a
venue that reports trades which later un-happen. Get it wrong in the
direction of being too cautious and you lose orders that were safely
replicated. Both are bad. Only one of them is bad in a way that ends up
in a regulatory filing.

## The core idea

Split the log into two regions with a single number.

**Committed** entries are held by a majority of the cluster. They will
survive any minority failure, and every future leader will have them.

**Uncommitted** entries exist on one node, or a minority. They may
survive. They may be silently deleted. Nobody has been told about them.

The rule that makes this workable is not about storage at all, it is
about what you say out loud:

> A client is told an order succeeded only after it is committed.

Once that holds, the awkward question stops being awkward. An
uncommitted entry disappearing is not data loss, because nothing ever
claimed it happened. A committed entry disappearing would be a genuine
failure, and the point of the design is that it cannot.

## How it works

The leader tracks, for each follower, the highest entry it has confirmed
holding. Sort those, take the value a majority reaches, and that is the
commit point.

```
5-node cluster, leader's view:

  node 1 (leader)  ####################  20
  node 2           ###############       15
  node 3           ##############        14
  node 4           #########              9
  node 5           ####                   4

  sorted:  20, 15, 14, 9, 4
  majority of 5 is 3, so take the 3rd:  14

  entries 1..14  committed. safe forever.
  entries 15..20 uncommitted. may vanish.
```

Two nodes are behind and it does not matter. That is the whole benefit
over requiring everybody: the cluster keeps working while a minority is
slow or dead.

Now the crash. The leader dies. Nodes 2 and 3 have everything through 14,
and one of them becomes leader. Entries 15 to 20 existed only on the dead
node and are gone. No client was told about them, so nothing was lost in
any sense that matters.

When the old leader restarts it finds a leader with a different history
past 14. It truncates its divergent tail and follows. Deleting entries
off your own disk sounds alarming and is completely safe, because they
were never committed.

## The key insight

**Majority commit alone is not sufficient, and the second half of the
argument is the part people skip.**

Committing on a majority guarantees that a majority *holds* an entry. It
does not, by itself, guarantee that whoever becomes leader next is one of
that majority. If a node missing committed entries can win an election,
those entries vanish and the guarantee was worthless.

So the election needs a matching rule: **a candidate whose log is behind
a voter's cannot get that vote.** Raft calls it the election restriction.
Together with majority commit it closes the loop:

- a committed entry is on a majority,
- any election needs a majority of votes,
- two majorities of the same cluster must overlap,
- so at least one voter has the entry,
- and that voter refuses any candidate lacking it.

Therefore every leader has every committed entry. The two rules are
useless separately and airtight together, and I did not appreciate that
until I had implemented one and found it insufficient.

**The second insight is stranger and it is the one I would want to be
asked about.**

A leader may **not** commit an entry from a previous term just because a
majority now holds it.

This felt obviously wrong when I first read it. A majority has the entry.
Majority means committed. What is the problem?

The problem is that "a majority holds it" is not the actual safety
property. The actual property is "no future leader can be elected without
it", and those come apart for old entries. Raft's Figure 8 walks through
a five-node schedule where an entry replicated to a majority by a new
leader, and committed on that basis, is later overwritten by a different
leader that was legally elected without it. The election restriction does
not save you, because the candidate's log was long enough to win on the
comparison while still missing that specific entry.

The fix is small and looks arbitrary until you see the counterexample: a
leader only advances the commit point to an entry from its **own** term.
Committing a current-term entry implicitly commits everything before it,
so old entries do become committed, just not directly.

Four lines of code. The difference between correct and wrong in a way
that would show up once a year under a schedule nobody could reproduce.

## The tradeoffs

**Latency, unavoidably.** A client cannot be told "done" until a majority
has the entry, which is at least one network round trip. You can pipeline
it so the caller rarely waits, but you cannot remove it. Anything
advertising both strong consistency and zero-round-trip acknowledgement
is doing one of them dishonestly.

**Uncommitted work is genuinely thrown away,** and this surprises people
operationally. A leader can accept a burst, die, and have all of it
vanish, and that is the system working correctly. The only defence is
never telling anyone it succeeded.

**Two-node clusters tolerate zero failures.** A majority of two is two. A
two-node cluster is no more available than a single node, it just costs
twice as much and fails in more interesting ways. This is why odd sizes
are the convention, and it is worth saying out loud because "we have a
backup" sounds like fault tolerance and is not.

**The commit point is one number for the whole log.** Simple to reason
about, and it means one slow follower cannot be routed around per-entry.

## How I used it in the project

Sententia Phase 5 replaced Phase 3's commit rule. That version committed
when **every** backup held an entry, which is stricter and simpler and
means a single dead backup stalls the cluster completely. Fine for a
demo, the opposite of fault tolerance.

Now the commit point is a majority, computed from each follower's
confirmed log head. The Figure 8 restriction is there too: the commit
point only advances to entries from the current term.

One implementation detail cost me an hour and is worth recording, because
it is a nice example of two rules being individually sensible and jointly
deadlocked. I first tracked each follower's **applied** index as its
match point. But a follower applies only what the leader has told it is
committed, and the leader cannot know something is committed until
followers report holding it. Each waits for the other and nothing is ever
committed. Everything simply hangs, with no error.

The fix is that the match point must be the follower's **log head**, not
its applied index. Holding an entry and having applied it are different
facts, and only the first one is what the commit calculation is about.

The test that convinced me it works builds a leader whose log runs ahead
of its commit point, kills it, recovers from disk, and asserts that
replaying only the committed prefix reproduces exactly the state both
nodes agreed on beforehand. Uncommitted entries survive recovery as log
entries, which is correct, and deciding whether they count is the
cluster's job rather than the disk's.

## What surprised me

**I had the wrong mental model of "committed" for a long time.** I
thought of it as a property of storage: written down, therefore
committed. It is a property of *agreement*. An entry fsynced to three
disks and known to nobody is less committed than an entry a majority has
acknowledged. Durability and commitment are different axes and I had them
collapsed into one.

**Deleting entries off a follower's disk felt wrong and is fine.** The
first time I implemented log truncation I put an assertion in that
refuses to truncate anything at or below the commit index, half expecting
it to fire. It cannot fire, and the reason it cannot is exactly the
overlap argument above. Writing the assertion was still the right call,
because if the safety argument ever fails somewhere I would rather crash
than corrupt.

**Figure 8 is the best argument I know for reading the paper rather than
the summary.** Every short explanation of Raft I had read said "commit on
a majority". That is the summary and it is wrong in a way you would never
discover from testing, because the schedule that breaks it involves a
specific five-node sequence of partial replications, crashes and
elections. I would not have invented that test. I only knew to handle it
because Ongaro drew the picture.

## References

- Ongaro and Ousterhout, *In Search of an Understandable Consensus Algorithm* (2014). Section 5.4 is the safety argument, and Figure 8 is the counterexample that makes the current-term rule necessary. Worth reading the figure caption three times.
- Ongaro, *Consensus: Bridging Theory and Practice* (2014), chapter 3, for the same material at more length.
- Howard, Schwarzkopf, Madhavapeddy, Crowcroft, *Raft Refloated* (2015), on reproducing and stress-testing these properties.
- Kingsbury, the Jepsen reports (jepsen.io). Many of them are exactly this failure: a system that reports something committed and then loses it.
- [`./raft-leader-election.md`](./raft-leader-election.md) - the election restriction, which is the other half of the argument here.
- [`./write-ahead-logging.md`](./write-ahead-logging.md) - how the entries get onto disk in the first place.
