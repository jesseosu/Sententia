# Field Notes: Building Leader Election

> What actually went wrong while implementing Raft-style election, in the order it happened. Three of the four problems were in my tests rather than my code, which turned out to be the most useful thing I learned.

**Written during:** Sententia Phase 4, leader election
**Tags:** `distributed-systems` `consensus` `raft` `testing` `debugging` `retrospective`

---

## Why write this separately

The Raft entry next door explains how election works. This one is the
logbook: the mistakes, what they cost, and what I changed so they cannot
happen again.

I am keeping them apart because they get read differently. The concept
entry is what I would want to explain in an interview. This one is what
I would want to reread before starting Phase 5, because the pattern in
it is more useful than any single bug.

The pattern, stated up front: **three of the four problems below were
wrong tests, not wrong code.** In a distributed system the test harness
is roughly as hard as the thing it tests, and I had not internalised that
before this phase.

---

## 1. I wrote down the wrong invariant

**What I believed:** the property to enforce is "at most one leader at
any instant". It is the obvious phrasing of "no split-brain" and it is
what I would have said out loud if asked.

**What happened:** my minority-partition test failed. Partition five
nodes into three and two, and sometimes a node on the two side was still
a leader. My first reaction was that I had a real split-brain bug and my
quorum arithmetic was broken somewhere.

**What was actually going on:** if the leader elected *before* the
partition happens to land in the minority, it stays leader. Nothing has
told it otherwise. It sends heartbeats into the void and gets no replies,
so it has no way to know it has been replaced. Meanwhile the majority
side notices the silence and elects a replacement in a higher term.

For a while there really are two nodes calling themselves leader.

That is not split-brain. They are in **different terms**, and the
isolated one cannot reach a majority, so it cannot commit anything. It is
a leader in name with no power. When the partition heals it sees the
higher term and stands down immediately.

**The fix:** the correct invariant is

> no *term* ever has two leaders

and my test now builds the partition so the existing leader is on the
majority side, then asserts the sharp thing: nobody in the minority ever
reaches leader, and `electionsWon` stays at zero for all of them.

**Why this is the most valuable mistake in the phase:** the gap between
"one leader at a time" and "one leader per term" is most of what makes
the protocol work. Raft does not prevent two nodes from believing they
lead. It makes the belief harmless, by ensuring the stale one cannot
assemble a quorum. I had read that. I had not understood it until a test
I wrote called correct behaviour a bug.

I also now think the shape of the error is common: writing an invariant
that is *stronger* than the real guarantee, then blaming the
implementation when reality fails to meet a bar nothing ever promised.

---

## 2. A decoder that silently dropped every reply

**Symptom:** after wiring terms into the replication messages, every
replication test failed at once. The backup had applied zero commands
while the primary had applied three thousand.

**Finding it:** I traced a single command through two nodes with print
statements. The backup received the leader's `AppendEntries`. The primary
received nothing back. So the reply was being sent and never arriving.

**Cause:** I added a `term` field to `AppendResponse`. The encoder wrote
it. The decoder did not read it.

The reason the decoder was missed is worth recording, because it is
mundane and it will happen again. My edit was a text substitution against
the decoder line, and `clang-format` had rewrapped that line at some
point, so the pattern no longer matched. The edit reported success and
changed nothing.

The consequence was that every `AppendResponse` decoded to garbage,
failed its trailing-bytes check, and got dropped as undecodable.
Replication stopped dead with no error visible anywhere except a log line
I was not reading.

**The deeper cause:** `AppendEntries` and `AppendResponse` were added in
Phase 3 and I never wrote round-trip codec tests for them. The other
message types had them. These two did not, purely because I added them in
a hurry and the integration tests passed.

**The fix:** read the term in the decoder, obviously. But more usefully,
I added round-trip coverage for every message type, and then a guard so
the gap cannot recur:

```cpp
constexpr std::size_t kMessageTypeCount = std::variant_size_v<Message>;
CHECK_EQ(kMessageTypeCount, std::size_t{8});
// ... one of each, encoded and decoded, compared for equality
```

Adding a ninth message type now fails that assertion until someone adds
coverage for it. The test knows how many message types exist and refuses
to be quietly incomplete.

**The lesson:** a serialisation format has two halves that must agree,
and nothing in the type system makes them agree. A round-trip test is the
only thing standing between you and a field that is written and never
read. It takes four lines. I skipped it once and it cost an hour.

---

## 3. Verifying that the split-brain test could detect split-brain

**Not a bug.** A habit that has now caught things twice, so it is worth
writing down as a practice.

After the simulator passed 7,744 checks on the first run I did not trust
it. A test that passes immediately on a subtle concurrent protocol is
more likely to be blind than correct.

So I broke the implementation on purpose, twice:

1. Changed `majority()` to return 1, so any single vote wins.
2. Removed the one-vote-per-term rule, so a node can vote for everybody.

Both produced immediate failures, naming exact seeds:

```
SPLIT BRAIN in term 1 (seed 4)
SPLIT BRAIN in term 1 (seed 7)
SPLIT BRAIN in term 1 (seed 12)
```

Restoring the code gave a clean pass again.

This is the same check I ran in Phase 1 (perturb an input, require the
output to change) and Phase 3 (make a replica skip one command in five
hundred, require divergence to be detected). It keeps earning its keep,
and it takes about five minutes.

The general form: **for any test asserting that something bad never
happens, make the bad thing happen and confirm the test notices.**
Otherwise all you have established is that the test compiles.

---

## 4. Chasing a flake I had already misdiagnosed

**Carried over from Phase 3 but it belongs here**, because I made the
same class of error twice and only spotted the pattern the second time.

A demo test was failing about one run in eight, and only when another
test run was happening concurrently. I assumed a port collision, changed
the demo configs to derive ports from the process id, and moved on.

The flake survived. The real cause was that the demo nodes ran for a
fixed number of poll cycles, and under CPU contention that budget could
expire before the cluster had converged. The test was comparing two
correct values captured at different points.

**The pattern I missed the first time:** I named a cause before measuring
one. Port collision was a plausible story that fit the symptom, and
plausible stories that fit the symptom are exactly what makes debugging
slow, because they feel like progress.

**The fix:** nodes gained `--exit-on-complete` and now end when the work
is genuinely finished rather than when a counter runs out. The cycle
budget stays purely as a hang guard. Ten concurrent rounds since, all
clean.

**Related, from Phase 2:** I once asserted that sending 4,000 messages
would produce at least one short write on loopback. It does not. It does
not at 50,000 either, because the kernel buffers absorb it. That
assertion was testing the local network stack, not my code.

Three separate instances of the same underlying mistake: **bounding a
test by something the environment controls.** Wall-clock budgets, cycle
budgets, and kernel buffer behaviour are all things that differ between
machines, and a test that depends on them fails somewhere else for
reasons that have nothing to do with the code.

---

## What I would tell myself before starting Phase 4

**Build the simulator before the protocol.** I wrote the election state
machine first and the deterministic cluster simulator second. In
hindsight the simulator is the more valuable artifact, and having it
first would have made the protocol work faster, because every rule could
have been validated the moment it was written.

**Keep the consensus code free of I/O for testing reasons, not
aesthetics.** I already believed in the pure-core split from Phases 1 and
2, but there it was mostly about clarity. Here it is the only reason the
split-brain test is possible at all. A cluster you can pause, partition,
crash and replay from a seed is a fundamentally different debugging
position from three processes and a network cable.

**Assume the harness is as hard as the system.** Three of four problems
this phase were in test code. That is not embarrassing, it is the
expected ratio for concurrent systems, and budgeting for it would have
made the phase feel less surprising.

**Write the invariant down and then attack the wording.** "At most one
leader" and "at most one leader per term" differ by two words and by the
entire correctness argument. Most of the value of this phase came from
being forced to notice the difference.

## References

- Ongaro and Ousterhout, *In Search of an Understandable Consensus Algorithm* (2014), section 5.2 and section 5.4.1.
- Kingsbury, the Jepsen reports (jepsen.io). The house style is exactly this: state the invariant precisely, then work very hard to violate it.
- [`./raft-leader-election.md`](./raft-leader-election.md) - how the protocol works, as opposed to how building it went.
- [`./state-machine-replication.md`](./state-machine-replication.md) - Phase 3, including the use-after-move that every correctness test passed through.
