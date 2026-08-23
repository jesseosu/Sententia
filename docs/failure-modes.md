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

## Open, deliberately

Things known to be wrong or missing, listed so they read as decisions.

- **No snapshots.** A backup that falls behind further than the retained
  log cannot catch up by replay. `CommandLog::canServeFrom` detects the
  condition and the replicator logs it plainly rather than sending a gap.
  Phase 5.
- **No durability.** The log is in memory. A primary that dies loses it.
  Phase 5.
- **No failover.** A backup never becomes a primary. Phase 4.
- **`compactLog` must be driven.** Nothing calls it automatically, so a
  long-running primary grows its log until something does.
- **No authentication or encryption.** A trusted network is assumed.
