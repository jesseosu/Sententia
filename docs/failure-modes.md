# Failure Modes

A running catalogue of the bugs and near-misses this project has hit, in
the order they were found. Each entry records the symptom, why it
happened, what fixed it, and what now catches it.

It is kept as a separate document because these span phases and keep
accumulating, and because the pattern across them is more instructive
than any one of them. Three themes have emerged so far:

1. **A correctness test can pass on a badly broken system.** Two of the
   defects below preserved correctness perfectly and were catastrophic
   anyway. Something has to assert cost, not just behaviour.
2. **A test that only passes when the environment cooperates is not
   testing what you think.** Two entries are wrong *tests* rather than
   wrong code.
3. **Bugs that only appear at scale are invisible at demo size.** The
   phase that introduces a load pattern is where the previous phase's
   unbounded thing finally bites.

---

## 1. Duplicate connections in a mesh

**Phase 2. Symptom:** every heartbeat delivered twice. The two-node demo
showed heartbeat counters repeating: `counter=2`, `counter=2`,
`counter=3`, `counter=3`.

**Cause:** in a mesh every node dials every other node. A dials B and B
dials A, both succeed, and the pair ends up with two TCP connections. All
traffic is duplicated.

**Why it mattered:** harmless for heartbeats. In Phase 3 it would have
meant applying every replicated command twice, and the resulting
divergence would have looked like a matching engine bug.

**Fix:** a tiebreak both sides compute independently, because a
negotiation would itself need a connection: **keep the connection dialed
by the lower-numbered node.** Node ids are unique and both sides know
both after the handshake, so both reach the same answer. A node also
declines to dial a peer it already has a live connection to, which stops
the loser from redialling forever.

**Caught now by:** `testMutualDialProducesOneConnection`, which asserts
exactly one ready peer per side, exactly one delivery per message, and no
flapping over 300 further cycles.

---

## 2. Accepted sockets do not inherit O_NONBLOCK

**Phase 2. Symptom:** a test hung forever with no error and no output.

**Cause:** a socket returned by `accept()` is blocking even when its
listener is non-blocking. POSIX does not propagate the flag. The
production path in `Transport::acceptPending` set it correctly; the test
that opened raw sockets did not, and its read loop blocked once it had
drained what was available.

**Why it mattered:** the failure mode is a hang rather than an error,
which from a CI timeout is nearly opaque.

**Fix:** set it explicitly on every accepted socket.

**Caught now by:** the comment and the explicit `setNonBlocking` call in
`testPartialWriteAgainstRealSocket`, plus every socket test being bounded
by iteration count rather than wall-clock time, so a hang shows up as a
failed assertion rather than a timeout.

---

## 3. Asserting a short write on loopback

**Phase 2. Symptom:** a test failed that should have passed. It asserted
`shortWrites > 0` after sending 4,000 messages.

**Cause:** the test was wrong, not the code. Measured: **zero short
writes even at 50,000 messages**, because the kernel's loopback buffers
absorb that much traffic without ever refusing a write.

**Why it mattered:** it was asserting a property of the local network
stack rather than of the transport. On a different machine it would have
passed or failed for reasons unrelated to the code.

**Fix:** stop hoping volume produces the condition and force it. Set
`SO_SNDBUF` to 4 KiB and write a megabyte; a short write and an `EAGAIN`
both appear within two iterations, every time.

**Caught now by:** the forcing function itself, plus `FrameWriter` drained
at every step size from 1 to 64 in `test_framing`, which covers the
buffer logic with no kernel involved at all.

---

## 4. Unbounded outbound queue

**Phase 2, found while assessing readiness for Phase 3. Symptom:** none
in Phase 2. That is the point.

**Cause:** `FrameWriter` had no cap, and `Transport::send` always
accepted. Once a peer stopped reading and the socket buffer filled, every
further send accumulated in memory forever.

Measured against a peer that was connected and alive but not consuming:

```
sent=  500000  bytes_written=4221579  queued_unsent=13278421
sent= 1000000  bytes_written=4264587  queued_unsent=30735413
sent= 1500000  bytes_written=4264587  queued_unsent=48235413
sent= 2000000  bytes_written=4264587  queued_unsent=65735413
```

Bytes actually written flatline at 4.26 MB. The queue grows linearly and
without bound: 65 MB and climbing.

**Why it mattered:** invisible in Phase 2, where the only traffic was
periodic heartbeats. Phase 3 replicates *every command* to the backup, so
a backup that pauses for a GC or a slow disk would kill the primary by
consuming its memory. A primary dying because a backup got slow is
exactly backwards for a fault-tolerance phase.

**Fix:** a high-water mark on `FrameWriter` (8 MiB default), and
`Transport::send` returning a `SendResult` that distinguishes
`WouldOverflow` from `NotConnected`. Phase 2 returned a bare `bool`,
which conflated "no such peer" with "peer is saturated" and left the
caller nothing to act on. The replicator refuses to submit while a backup
is saturated, so backpressure reaches the client rather than the heap.

After the fix, the same reproduction:

```
sent= 2000000  ACTUAL_queue_bytes=8388624  peak=8388624  accepted=225913 refused=1774087
```

Flat at the high-water mark plus one message, since the check happens
before the enqueue.

**Caught now by:** `test_backpressure`, which asserts the peak queue stays
under the mark, that refusals are reported rather than silent, and that
the sender recovers once the peer resumes reading.

---

## 5. Use-after-move in the replication cursor

**Phase 3. Symptom:** every correctness test passed. The benchmark did
not finish.

**Cause:**

```cpp
const SendResult result = transport_.send(backup.id, Message{std::move(msg)});
if (!probe && !msg.entries.empty()) {      // msg was moved from
    backup.lastSent = msg.entries.back().seq;
}
```

Moving `msg` into the `Message` variant leaves its `entries` vector
empty. So `msg.entries.empty()` was always true afterwards, and
`backup.lastSent` stayed permanently at zero. Every submit re-sent the
entire outstanding range from the beginning.

Replicating **1,000 commands** cost:

| | before | after |
|---|---|---|
| messages sent | 883,003 | 1,001 |
| bytes sent | 91,478,538 | 72,598 |
| throughput | 78 cmd/s | 91,338 cmd/s |

Roughly 880x the messages and 1,260x the bytes.

**Why it mattered, and why nothing caught it:** re-sending is harmless to
*state*. The backup detects the overlap, applies what is new, and
converges to exactly the right checksum. Every correctness assertion in
`test_replication` passed, including the chaos test. The system was
provably correct and completely unusable, and only a benchmark could tell
the difference.

This is the sharpest lesson so far. A test suite that only checks
behaviour cannot distinguish a good implementation from a catastrophic
one, as long as the catastrophic one is *correct*.

**Fix:** capture the values needed for bookkeeping before the move.

**Caught now by:** `testNoMessageAmplification`, which asserts entries
sent, messages sent, and bytes sent all stay proportional to the command
count. The benchmark also prints `entries/cmd`, `msgs/cmd` and
`bytes/cmd` next to throughput, so amplification is visible in the
headline output rather than buried.

---

## 6. Broken demo assertions after a binary grew a new job

**Phase 3. Symptom:** the Phase 2 `two_node_demo` test failed after the
node binary gained replication.

**Cause:** the node's message handler started routing everything to the
replicator, so it stopped logging `RECV ... HEARTBEAT`, and its shutdown
summary was rewritten around replication counters, dropping
`framing_errors`. Both were things the Phase 2 demo asserted on.

**Why it mattered:** the temptation was to relax the older test, which
would have quietly retired real Phase 2 coverage to make a Phase 3 change
convenient.

**Fix:** restore both outputs instead. The node now routes only
`AppendEntries` and `AppendResponse` to the replicator, logs everything
else, and reports transport stats alongside replication stats. Better
output than before, and the older guarantee stays guarded.

---

## 7. A demo bounded by a cycle budget instead of by being finished

**Phase 3. Symptom:** the full suite failed roughly one run in eight, and
only when another `ctest` was running at the same time. Eight consecutive
runs in isolation were clean, which is the worst possible signal.

**Cause:** the replication demo ran its nodes with `--max-cycles 120`.
Under CPU contention the poll loop still burns cycles while the processes
are descheduled and the connection is slow to establish, so the budget
could run out before the backup had converged. The demo then compared two
checksums that were correct but taken at different points.

**Why it mattered:** it is the same mistake as asserting a short write on
loopback, one entry up. A test bounded by something the environment
controls is not testing what you think, and an intermittent failure that
only appears under load is far more expensive than an outright one.

The first fix attempt was also wrong. Assuming a port collision, the demo
configs were changed to derive ports from the process id. That was worth
doing anyway, but it was not the bug, and the flake survived it. Naming a
cause before measuring is how you fix the wrong thing.

**Fix:** the node gained `--exit-on-complete`, and the demos now end when
the work is actually done: for a primary, everything submitted, applied,
and acknowledged by every backup; for a backup, the primary having
finished and disconnected. `--max-cycles` stays as a safety net so
nothing can hang, but it is no longer what normally ends a run.

**Caught now by:** ten consecutive rounds of Release and Debug `ctest`
running concurrently, all clean, where the previous arrangement failed
about one round in eight.

---

## 8. An invariant that was stronger than the guarantee

**Phase 4. Symptom:** the minority-partition test failed. A node on the
two-node side of a five-node partition was a leader.

**Cause:** the test, not the protocol. If the leader elected before the
partition lands in the minority, it stays leader in its **old term**,
because nothing has told it otherwise. The majority elects a replacement
in a higher term. Two nodes then call themselves leader.

That is not split-brain. They are in different terms, and the isolated
one cannot reach a majority so it cannot commit anything.

**Why it mattered:** the assertion "at most one leader at any instant" is
stronger than what Raft guarantees, and it fails on correct behaviour.
The real invariant is "no term ever has two leaders". The gap between
those two statements is most of the correctness argument.

**Fix:** build the partition so the existing leader is on the majority
side, then assert the sharp property: no node in the minority ever
reaches leader, and `electionsWon` stays zero for all of them.

**Caught now by:** `testMinorityPartitionCannotElectALeader`, plus the
continuous `assertNoSplitBrain` check that uses the correct per-term
invariant.

---

## 9. A decoder that silently dropped every reply

**Phase 4. Symptom:** every replication test failed at once after adding
terms to the replication messages. The backup had applied 0 commands
while the primary had applied 3,000.

**Cause:** a `term` field was added to `AppendResponse`. The encoder
wrote it; the decoder did not read it. The edit that should have updated
the decoder was a text substitution that no longer matched, because
`clang-format` had rewrapped the line. It reported success and changed
nothing.

Every `AppendResponse` then decoded misaligned, failed its
trailing-bytes check, and was discarded as undecodable. Replication
stopped dead, with the only evidence a log line nobody was reading.

**The deeper cause:** `AppendEntries` and `AppendResponse` were added in
Phase 3 with no round-trip codec coverage. Every other message type had
it.

**Fix:** read the field. More usefully, round-trip coverage for every
message type plus a guard that fails if a new one is added without it:

```cpp
constexpr std::size_t kMessageTypeCount = std::variant_size_v<Message>;
CHECK_EQ(kMessageTypeCount, std::size_t{8});
```

**Caught now by:** `test_message_codec`, which now round-trips all eight
message types and refuses to be quietly incomplete.

**The general lesson:** a serialisation format has two halves that must
agree and nothing in the type system makes them. A round-trip test is the
only thing standing between you and a field that is written and never
read.

---

## 10. An edit that reported success and changed nothing, twice

**Phase 4. Symptom:** the "Open, deliberately" list below still claimed
"No failover. A backup never becomes a primary. Phase 4" *after* Phase 4
had shipped working failover. The Phase 4 gaps that were supposed to
replace it were absent entirely.

**Cause:** the same one as defect 9, one document later. The update was a
text substitution anchored on two adjacent lines, and a third bullet sat
between them, so the pattern did not match. The tool reported success. It
changed nothing.

**Why it mattered:** less than the decoder bug, because stale docs do not
corrupt data. But it was caught only because somebody asked "are these
actually resolved?" and the honest answer required rereading rather than
remembering. A catalogue of known gaps that silently goes stale is worse
than no catalogue, because it is trusted.

The compounding detail: this is the **third** instance this phase of an
edit whose anchor drifted, and it landed in the document whose entire job
is recording that class of mistake.

**Fix:** the missing content, plus a habit. Substitution-based edits now
assert their anchor exists before rewriting, so a stale pattern is a hard
failure rather than a silent no-op:

```python
assert old in s, "anchor not found"
s = s.replace(old, new)
```

Every other Phase 4 documentation and build edit was then audited the
same way: 16 markers checked, 15 present, 1 missing. This was the one.

**The general lesson:** an edit that cannot fail is an edit you cannot
trust. Whether the tool is `sed`, a script, or a person, "I changed it"
and "it changed" are different claims, and only the second one matters.
Verify the change landed, not that the command exited zero.

---

## 11. Two correct rules that deadlocked together

**Phase 5. Symptom:** the entire suite hung after switching to majority
commit. No error, no crash, no output.

**Cause:** each follower's **applied** index was used as its match point
for the commit calculation. But a follower applies only what the leader
has told it is committed, and the leader cannot know an entry is
committed until followers report holding it. Each waits for the other.

**Why it was hard:** both rules are individually correct. Followers must
apply only committed entries, or a client can observe a trade that later
un-happens. Commit means what a majority holds. The bug lives only in the
interaction, and the symptom is silence.

**Fix:** the match point is the follower's **log head**, not its applied
index. Holding an entry and having applied it are different facts.

**Caught now by:** every replication test, which hang without it.

---

## 12. A follower permanently one entry behind

**Phase 5. Symptom:** replication converged for every entry except the
last, forever.

**Cause:** the leader learns a follower holds entry N, commits N, then
has nothing left to send, so the new commit point is never advertised.
Invisible mid-burst because the next entry carries it; fatal at the end
of a burst.

**Fix:** track the commit point each follower has been told and send an
empty append when it moves. Real Raft folds this into the heartbeat.

**Cost, measured:** messages per command went from 1.00 to 2.00, entries
per command stayed at 1.00, so nothing is re-sent.

---

## 13. A test that was structurally incapable of failing

**Phase 5, and the most instructive one here. Symptom:** none.
`testSnapshotEntriesAreNotAppliedTwice` passed, and kept passing when the
boundary check was deliberately changed from `<=` to `<`.

**Cause:** the snapshot function truncated the **entire** log, so after a
snapshot no records at or before the boundary existed, no overlap
occurred, and the skip logic never ran. The test set up a world where the
bug could not manifest and confirmed it did not manifest. Its comment
confidently described an overlap the setup made impossible.

**Two fixes, because two things were wrong.** Truncating the whole log is
only safe when the snapshot is at the log head, so truncation became
partial. And the test now builds the realistic overlap: a crash between
writing the snapshot and truncating the log, which is a real window
created by writing the snapshot first (the safe order). It asserts
exactly 500 entries replayed, not 501.

**Caught now by:** `testCrashBetweenSnapshotAndTruncationDoesNotDoubleApply`,
verified to fail on the off-by-one.

**The lesson:** "does this test pass" and "can this test fail" are
different questions and only the second is worth much.

---

## 14. An accessor correct only immediately after loading

**Phase 5. Symptom:** partial truncation kept nothing; recovery reported
zero entries replayed instead of 500.

**Cause:** the WAL kept an in-memory vector of records populated during
replay at open and never updated on append. Within a session it stayed
empty however much was written.

**Fix:** append updates the mirror.

**The lesson:** a view of an object's contents should be correct at every
point in its life, not just right after construction. The next caller of
a load-only-correct accessor is always you, twenty minutes later.

---

## 15. Idempotency guards keyed on non-unique strings

**Phase 5, and the third variant of defect 9 and 10.** Substitution edits
were guarded with `if "lastLogSeq" not in source` to avoid applying twice.
That field already existed on a different message type, so the guard saw
it, concluded the edit was done, and skipped. Twice, on two files.

**Fix:** stop guarding on substrings that are not unique to the thing
being added. Assert the precise anchor, apply, verify afterwards.

**The pattern across Phases 4 and 5:** five instances of a tool reporting
success without doing the work, every one found by a compiler or a test
rather than by reading the diff. The countermeasure that works is making
the tool unable to fail quietly, not resolving to be more careful.

---

## 16. A demo that finished before the cluster did

**Phase 5. Symptom:** one full-suite run in eight failed on
`replication_demo_sync`.

**Cause:** the leader's completion condition checked only its own applied
index. Under the new commit rule the leader can be caught up while a
follower is still one commit-advertisement behind, so it exited and the
follower reported a smaller state.

**Fix:** "done" in a replicated system means the cluster is done. The
condition now requires every follower to have applied everything too.

**Caught now by:** eight consecutive demo runs plus repeated full-suite
runs, where the previous arrangement failed about one in eight.

---

## 17. The same instantaneous assertion mistake, twice in two minutes

**Phase 5. Symptom:** `test_backpressure` failed about one Debug run in
four on `CHECK(a.isSaturated(2))`.

**Cause:** saturation is an instantaneous condition, and `send()` now
drains any writable backlog before checking it, so the kernel may have
accepted enough bytes on the final call to drop the queue below the
high-water mark. The assertion was sampling a value the environment
controls.

**What makes this worth an entry:** I diagnosed it correctly, wrote a
paragraph explaining that instantaneous conditions must not be asserted,
and then replaced it with `CHECK(a.pendingBytes(2) > 0)`, which is the
identical mistake one line lower. It flaked at exactly the same rate.

**Fix:** assert nothing about the queue at a single instant. The property
under test is that the queue stays **bounded** and that refusals are
reported, and `peakPending` (accumulated across the whole loop),
`refused`, and `sendsRefusedOverflow` already establish both. Twenty
consecutive Debug runs clean afterwards, where the previous two versions
failed three in twelve.

**The pattern, now on its fourth appearance** (short writes on loopback,
demo cycle budgets, and both of these): bounding a test by something the
environment controls. Knowing the rule did not stop me applying the
anti-pattern again while writing the comment that explains it.

---

## 18. Four invariants nothing was guarding, found in one command

**Post-Phase-5. Symptom:** none. Everything was green.

Defects 13 and 17 were both caught by manually breaking the code and
checking a test noticed. That worked, and it depended entirely on
remembering to do it, once per phase, on whichever code felt risky. It
missed things for a whole phase at a time.

So the sabotage check became a script: `scripts/mutation_check.py`, a
registry of 20 named mutations, each attacking a property the project
actually claims, each naming the test that must fail.

**The first run killed 15 of 20.** The four survivors were invariants
with no test behind them:

| Survivor | The unguarded property |
|----------|------------------------|
| `commit-ignores-current-term-rule` | **Raft Figure 8.** The subtlest correctness rule in the project, documented at length, tested by nothing. |
| `replication-ignores-log-matching` | A follower checking the term at `prevSeq`, not just the sequence. Nothing ever produced divergent logs. |
| `replication-skips-a-command` | See below. |
| `wal-ignores-short-payload` | A bounds check. Removing it read past a buffer and every assertion still passed. |

Plus one registry entry that would not compile, which the harness
correctly reported rather than skipping.

**Fixes:** a Figure 8 test that constructs the multi-term situation
directly, a log-matching test that injects a term conflict, an
ASan/UBSan build (assertions cannot see memory errors), and the oracle
described next. All 20 mutations are now killed.

---

## 19. Agreement is not correctness

**Post-Phase-5. Symptom:** `replication-skips-a-command` survived. Making
the apply loop drop one command in five hundred left every convergence
test green.

**Cause:** every replication assertion compared the leader against the
follower and nothing else. Both nodes ran the same mutated code, skipped
the same commands, and agreed perfectly on a book that was wrong.

The tests checked **agreement**. Agreement is not correctness, and the
distinction is invisible until something breaks both sides identically.

**Fix:** a third party. Convergence tests now also compare against a
single-process engine applying the same commands with no replication
involved at all.

**The general lesson:** whenever two things are checked against each
other, ask what happens if both are wrong the same way. In a replicated
system that question has teeth, because making both sides run identical
code is the entire point.

---

## 20. A mutation that landed on dead code

**Post-Phase-5.** After adding the oracle, `replication-skips-a-command`
*still* survived.

**Cause:** the mutation anchor matched two places, and the first was
`Replicator::applyThrough`, a function left over from the Phase 3 commit
model that nothing had called since. The mutation dutifully broke dead
code and no test noticed, which is correct behaviour and completely
uninformative.

**Two fixes.** The dead function is gone. And an anchor matching more
than one place is now a hard failure in the harness rather than a silent
first-match: a mutation that can land anywhere can land somewhere
harmless, and the result is indistinguishable from a test that would have
caught it.

**Worth noting:** this is the same failure shape as defects 9, 10 and 15,
where an edit silently matched nothing. Here it silently matched the
wrong thing. Both are a tool doing something other than what was intended
and reporting success, and the countermeasure is the same: make ambiguity
an error rather than a coin flip.

---

## 21. A number quoted as evidence for five phases, asserted nowhere

**Post-Phase-5. Symptom:** none. Prompted by being asked whether the
unchanging replay checksum was a concern.

For five phases every status report ended with the same reassurance:
Phase 1's replay checksum is still `17373410596180621386`, therefore
nothing regressed. The checksum being stable was correct and expected,
since later phases add transport, replication, election and durability
without touching matching semantics.

**The problem was the evidence, not the number.** That value was asserted
in no test anywhere. It was being compared by eye across messages.

The `replay_sample` test that looked like it guarded this only ran the
replay driver twice and checked the two runs agreed **with each other**.
Change the matching rules and both runs change together, agree perfectly,
and the test still passes. Exactly the trap defect 19 describes, sitting
undetected in the one check most often cited as proof nothing had broken.

Engine behaviour was not actually unguarded, because `test_determinism`
pins a golden 22-event stream. But the specific claim being repeated had
nothing behind it.

**Fix, in two parts.**

The three checksums are now pinned as golden values passed into the test,
so a semantics change fails rather than drifts. Verified by feeding a
deliberately wrong expected value and confirming it fires.

And the sample order file was strengthened, because pinning a weak input
pins very little. Every crossing in the old file happened at an identical
price, so it could not have detected the execution-price rule changing at
all: a mutation trading at the aggressor's price instead of the resting
price left its output byte-identical. The file now includes a genuine
price-improvement crossing (a buyer at 110 meeting an ask at 105, which
must print at 105) and two resting orders at one price to exercise FIFO.

With that, the mutation `engine-trades-at-the-wrong-price` is killed by
`replay_sample`. Before, it was invisible to it.

**The lesson, and it is uncomfortable:** the reassurance was being
generated by the same process that would have missed the regression. A
number repeated confidently in a status report is not a test. If it is
worth quoting as evidence, it is worth asserting, and if the input behind
it is trivial then the assertion is worth very little either way.

---

## 22. CI red for five commits while every report said green

**Post-Phase-5. Symptom:** the branch had a failing check on GitHub from
the Phase 4 commit onward. Five consecutive pushes, all reported here as
verified and green.

**Cause, in two parts.**

The *reporting* failure is the serious one. Every phase was verified
locally on Linux with GCC and Clang, Release and Debug, and reported as
such truthfully. GitHub Actions also builds on **macOS and Windows**, and
that was never once looked at. "Verified on the platforms I ran" was
stated as if it were "verified", and the gap between those went
unmentioned because it went unnoticed.

The *technical* failures were two genuine bugs, both macOS-only:

**`fdatasync` does not exist on macOS.** Introduced in Phase 5 and broke
the build outright. Now `#if defined(__APPLE__)` uses `fsync`. Worth
noting while there: on Apple hardware even `fsync` only pushes to the
drive rather than through its write cache, and `F_FULLFSYNC` is the
stronger primitive a real venue would want, at a real cost.

**A read loop that gave up at the first `WouldBlock`.** From Phase 4:

```cpp
if (r.status == IoStatus::WouldBlock) {
    break;   // assumes the bytes have already arrived
}
```

On Linux loopback they have. On macOS they may not be there yet, so the
loop exited having read nothing and `received > 0` failed. Now it
retries, bounded by iterations.

That is the **fifth** instance of an assertion bounded by something the
environment controls, and the first one to be caught by a platform
rather than by luck. The pattern held: knowing the rule did not prevent
it, and the thing that eventually found it was a machine that behaved
differently.

**Fix, beyond the two bugs:** CI results are now checked before a phase
is reported complete, not just the local suite. `make flake` and `make
mutants` were built to check the tests; nothing was checking that the
checks themselves were passing where they actually run.

---

## Open, deliberately

Things known to be wrong or missing, listed so they read as decisions.

- **Snapshots are not automatic.** Nothing takes one on a timer or size
  threshold; an operator has to drive it.
- **InstallSnapshot is not a wire message.** The mechanism exists and is
  tested, but a leader does not push a snapshot to a lagging follower
  over the network. The condition is detected and reported rather than
  repaired automatically.
- **The WAL is replayed linearly with no index**, so opening a very large
  log is O(size), and it keeps every record in memory. Bounded in
  practice by snapshotting, wrong for a log larger than RAM.
- **No whole-file checksum on the WAL**, only per record. A record that
  verifies individually inside an otherwise corrupted file is trusted.
- **No pre-vote.** A node returning from a partition bumps the term and
  briefly disrupts a healthy leader. Raft's pre-vote extension avoids it.
- **No leadership transfer** on clean shutdown. A leader shutting down
  could hand over rather than letting the cluster time out.
- **No membership changes.** The cluster is fixed at startup. Joint
  consensus is a hard problem in its own right.
- **`compactLog` must be driven.** Nothing calls it automatically, so a
  long-running primary grows its log until something does.
- **No authentication or encryption.** A trusted network is assumed.
