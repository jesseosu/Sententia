# Field Notes: Making It Survive a Crash

> What went wrong building durability and recovery, in order. The theme this time: the tests were the hard part again, and two of them were passing while being incapable of failing.

**Written during:** Sententia Phase 5, fault-tolerant recovery
**Tags:** `durability` `crash-recovery` `testing` `debugging` `retrospective`

---

## Why this is separate

The other two Phase 5 entries explain how write-ahead logging and the
commit rule work. This is the logbook: what actually broke, what it cost,
and what I changed so it cannot happen again.

The pattern that ran through this phase: **a test passing tells you very
little unless you have watched it fail.** Two tests here were passing
while structurally unable to detect the bug they were named after. I only
found that by deliberately breaking the code.

---

## 1. Two rules that each made sense and deadlocked together

**Symptom:** after switching to majority commit, the whole test suite
hung. No error, no crash, no output. Just nothing, forever.

**Cause:** I tracked each follower's **applied** index as its match point
for the commit calculation. That seems obviously right: how far has this
follower got.

But a follower applies only what the leader has told it is committed, and
the leader cannot know an entry is committed until followers report
holding it. So the leader waits for the follower to apply, the follower
waits for the leader to commit, and neither ever moves. The commit point
sits at zero forever.

**The fix:** the match point has to be the follower's **log head**, not
its applied index. Holding an entry and having applied it are different
facts, and the commit calculation is about the first one.

**What made it hard:** each rule is individually correct. "Followers
apply only committed entries" is a genuine safety requirement, and
without it a client can observe a trade that later un-happens. "Commit
what a majority holds" is the definition. The bug only exists in the
interaction, and the symptom is silence.

**How I found it:** stopped reasoning and printed the state of both nodes
after each command. Two lines of output made it obvious: the follower's
log head advanced and its applied index never did.

---

## 2. A follower one entry behind, forever

**Symptom:** after fixing the deadlock, replication converged for every
entry except the last one. The follower always sat exactly one behind.

**Cause:** the leader learns a follower holds entry N, commits N, and
then has nothing left to send. The new commit point is never advertised,
so the follower never applies the entry it already has. During a burst
this is invisible, because the next entry carries the updated commit
point along with it. At the end of a burst there is no next entry.

**The fix:** track what commit point each follower has been told, and
send an empty append when it moves. Real Raft folds this into the
periodic heartbeat, which is why it never shows up as a separate problem
there. I had put heartbeats in the election layer and replication had no
equivalent.

**Cost:** exactly one extra message per command, measured. Entries per
command stayed at 1.00, so nothing is re-sent, and messages per command
went from 1.00 to 2.00. That is real work rather than waste, and I
updated the amplification test bound with the measurement and the
reasoning rather than just raising the number until it passed.

---

## 3. A test that could not fail

**This is the one worth reading.**

I had a test called `testSnapshotEntriesAreNotAppliedTwice`. It described
the classic snapshot-plus-log bug: replay entries the snapshot already
contains and the book is silently wrong. It passed.

Following the habit from earlier phases, I broke the boundary check on
purpose, changing `entry->seq <= startAfter` to `<`. An off-by-one that
would re-apply exactly one command.

The test still passed.

**Why:** my snapshot function truncated the **entire** log. So after a
snapshot there were no records at or before the boundary, no overlap
existed, and the skip logic never ran. The test had set up a world in
which the bug could not occur and then confirmed it did not occur.

Not a weak test. A vacuous one. And the comment above it confidently
described an overlap that the setup made impossible, which is worse,
because it reads as coverage.

**The fix had two parts.**

First, the code was wrong too. Truncating the whole log is only safe when
the snapshot is taken at the log head. A snapshot taken behind the head
must keep everything after it, so truncation became partial.

Second, the test needed a scenario where the overlap genuinely exists.
The realistic one is a crash **between writing the snapshot and
truncating the log**. That ordering is deliberate (snapshot first, or a
crash in between loses everything), and its cost is exactly this overlap
window. So the test now constructs that on-disk state directly and
asserts that precisely the entries after the boundary are replayed: 500,
not 501, not 1500.

With that in place the off-by-one is caught immediately.

**What I take from it:** "does this test pass" and "can this test fail"
are different questions, and only the second one is worth much. I have
been running the sabotage check since Phase 1 and it has now caught
something in four phases running. This time it caught a problem in the
test rather than the code, which is a new one.

---

## 4. An accessor that was only correct right after loading

**Symptom:** once truncation became partial, it kept nothing. Recovery
reported zero entries replayed when it should have reported 500.

**Cause:** my write-ahead log kept an in-memory vector of records,
populated during replay at open time and **never updated on append**. So
within a single session it stayed empty no matter how much was written,
and the filter that decided what to keep saw an empty list.

**The fix:** append updates the mirror.

**The lesson:** an accessor that is correct only immediately after load is
a trap for whoever calls it next, and the next caller is always you,
twenty minutes later. If a class exposes a view of its contents, that
view should be right at every point in its life, not just at the
beginning.

---

## 5. My edit tooling silently doing nothing, again and again

**Carried over from Phase 4, where it appeared twice.** It appeared three
more times this phase.

Phase 4 ended with me adding assertions to substitution-based edits so a
stale pattern fails loudly rather than reporting success and changing
nothing. That helped. It caught two drifted anchors immediately.

Then I introduced a new variant of the same mistake. To avoid applying an
edit twice I started guarding them:

```python
if "lastLogSeq" not in source:
    ...apply the edit...
```

`lastLogSeq` already existed on a different message type. The guard saw
it, concluded the edit was already applied, and skipped. Twice, on two
different files, before I noticed that the field I was adding was simply
absent from the struct while the compiler insisted it was there elsewhere.

**The fix:** stop guarding on substrings that are not unique to the thing
being added. Assert the precise anchor exists, apply, and verify the
result afterwards.

**The pattern across both phases:** every one of these was a tool
reporting success without doing the work, and every one was found by the
compiler or a test rather than by me reading the diff. The countermeasure
that actually works is not being more careful, it is making the tool
unable to fail quietly.

---

## 6. A demo that finished before the cluster did

**Symptom:** one full-suite run in eight failed on `replication_demo_sync`.

**Cause:** the leader's "work is done" condition checked only its own
applied index. Under the new commit rule the leader can be fully caught
up while the follower is still one commit-advertisement behind, so the
leader exited and the follower then reported a smaller state.

**The fix:** "done" in a replicated system means the **cluster** is done.
The condition now also requires every follower to have applied everything.

**Why it is worth recording:** it is not a test artifact, it is the real
distributed-systems boundary showing up in a convenient place. Any code
that asks "have we finished" in a cluster has to decide whose finish it
means, and the local answer is almost never the interesting one.

---

---

## 7. Making the same mistake while writing the comment explaining it

A backpressure test asserted `isSaturated(peer)` at the end of a loop. It
flaked about one Debug run in four, because saturation is instantaneous
and the send path drains any writable backlog before checking, so the
kernel could accept enough bytes on the final call to drop below the
mark.

I diagnosed that correctly. I wrote a paragraph explaining that
instantaneous conditions must not be asserted because the environment
controls them. Then I replaced the assertion with `pendingBytes(peer) > 0`,
which is the same mistake one line lower, and it failed at the same rate.

The fix was to assert nothing about the queue at a single instant. The
real property is that the queue stays bounded and refusals get reported,
and a peak accumulated across the whole loop already establishes it.

This is the fourth time in five phases I have bounded a test by something
the environment controls: short writes on loopback, a demo cycle budget,
and both halves of this one. Knowing the rule is clearly not the same as
applying it, and the thing that actually catches it is noticing that an
assertion reads a value rather than a summary. A peak, a count, a total:
fine. A snapshot of right now: suspect.

## What I would tell myself before starting Phase 5

**Write the crash test before the crash handling.** I built the WAL, then
tested it. Building the "kill it at a bad moment and see" harness first
would have shaped the design, and it is the same lesson as Phase 4, where
the simulator turned out to be more valuable than the protocol.

**Snapshots are where the subtle bugs live, not logs.** The log is
simple: append, checksum, replay, truncate a torn tail. The snapshot
interacts with the log, and every interaction is an ordering question
with a wrong answer that only fails on a crash in a specific window.

**Assume any test involving a boundary is wrong until you have seen it
fail.** Off-by-ones at a snapshot boundary, at a commit point, at the
handover from catching up to live: three chances to be quietly wrong, and
none of them fail loudly on their own.

**Budget for the tests being harder than the feature.** Phase 4 taught me
this and I did not internalise it enough. Roughly two thirds of the time
here went into test infrastructure and into discovering that tests were
not testing what they claimed.

## References

- Pillai et al., *All File Systems Are Not Created Equal* (OSDI 2014). The paper that makes you fsync directories, and a good corrective to assuming rename is enough.
- Ongaro and Ousterhout, *In Search of an Understandable Consensus Algorithm* (2014), section 7 for snapshotting alongside a replicated log.
- [`./write-ahead-logging.md`](./write-ahead-logging.md) - how the durability layer works.
- [`./committed-and-uncommitted.md`](./committed-and-uncommitted.md) - what a leader crash may and may not lose.
- [`./phase-4-field-notes.md`](./phase-4-field-notes.md) - the previous phase's logbook, where the edit-tooling problem started.
