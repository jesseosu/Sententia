# State Machine Replication

> Do not copy the state. Copy the instructions that produced it, and let the other machine do the work. The whole design rests on the machine being deterministic, which is why that had to be built first.

**Learned during:** Sententia Phase 3, primary to backup replication
**Tags:** `distributed-systems` `replication` `consistency` `cap-theorem` `state-machine-replication`

---

## The problem it solves

A primary node holds an order book and processes orders against it. A backup needs the same book, so that when the primary dies the backup can take over without anyone noticing a gap.

The obvious approach is to send the book. After every order, ship the current state to the backup.

It is correct and it is unusable. An order is a few dozen bytes; a book with eighty thousand resting orders is several megabytes. The cost per order would scale with the size of the book rather than the size of the change, so replication would be most expensive exactly when the venue is busiest and the book is deepest. That is precisely backwards.

Shipping diffs instead of whole books helps, but not as much as it sounds. You now maintain a diff format, and the diff of an order that sweeps four price levels is not obviously smaller than the four events describing it. You are also still shipping *consequences* rather than *causes*, which leaves the backup a passive recipient with nothing it can independently verify.

## The core idea

Send the commands. Let the backup compute the state itself.

If the backup applies the same commands in the same order as the primary, and the engine is deterministic, it arrives at the same state without either node ever having described that state to the other. The message stays a few dozen bytes no matter how deep the book gets. Measured in Sententia: 72.6 bytes per command, flat.

This is state machine replication. It is the idea underneath Raft, underneath Paxos, underneath Kafka's log, underneath every database that replicates by shipping its write-ahead log rather than its pages.

It has exactly one precondition: the state machine must be deterministic. Same starting state, same command sequence, same result, every time, on every machine. If that does not hold, the two nodes silently drift apart and nothing detects it until a failover promotes a replica whose book disagrees with the one it replaced.

## How it works

```
   client command
        |
        v
   +---------+   assign seq N          +---------+
   | PRIMARY | ----------------------> | BACKUP  |
   |         |   AppendEntries         |         |
   | log[N]  |   prevSeq=N-1           | log[N]  |
   |         | <---------------------- |         |
   +---------+   AppendResponse        +---------+
        |        lastApplied, checksum      |
        v                                   v
     state S                             state S
```

Four mechanisms make it work.

**Sequence numbers.** The primary assigns every command a monotonic, gapless number. These are distinct from any counter the engine keeps internally; the engine's counter is an implementation detail, while these are what two machines agree on.

**Gap detection.** Each batch says which sequence the backup must already have applied. If it does not match, the backup refuses the batch and reports where it actually got to. Applying out of order would break determinism silently and unrecoverably, so it is refused rather than repaired.

**Catch-up.** A backup that reconnects may have applied anything from nothing to everything, and the primary cannot know which. So it probes with an empty batch, the backup reports its position, and the primary resends from there.

**Checksums.** Every response carries a hash of the backup's state. When both have applied to the same sequence, the hashes must match. Comparing one integer is what makes divergence detection cheap enough to run on every message, and it only works because the hash is a function of *state* rather than of the history that produced it.

## The key insight

Two, and the second one caught me out.

**The first: order is as much a part of the input as the commands are.** I had internalised "same commands, same state" and it is half the story. Swapping two adjacent commands changes the outcome, which Sententia's Phase 1 sensitivity test asserts directly. So the log's ordering is not a convenience of the transport, it is part of what has to be agreed.

That reframes leader election completely. I had thought of it as a reliability feature: the cluster needs someone in charge so it can survive losing them. It is more fundamental than that. With no agreed primary there is no agreed order, and with no agreed order there is no agreed state. Election is not bolted on to make replication robust; it is what makes replication *well-defined* once more than one node can accept work. Raft's leader exists to impose a total order, and the fault tolerance is a consequence.

**The second: synchronous versus asynchronous is not a binary.** The textbook framing is two options. Wait for the backup to acknowledge before telling the client "done", and you never lose an acknowledged command but you pay a network round trip. Do not wait, and you are fast but a crash can lose whatever had not arrived.

I implemented both and measured them, and the interesting number was not the one I expected:

| Mode | In flight | Throughput | Submit p50 |
|------|-----------|-----------|-----------|
| Synchronous | 1 | 8,183 cmd/s | 61,325 ns |
| Synchronous | 8 | 8,264 cmd/s | 2,447 ns |
| Synchronous | 64 | 8,277 cmd/s | 2,432 ns |
| Asynchronous | unbounded | 15,692 cmd/s | 2,787 ns |

Strict lockstep costs the caller 61 microseconds per command. Allowing eight commands in flight drops that 25x, to 2.4 microseconds, while throughput barely moves.

And the guarantee is unchanged. A command is still only committed once acknowledged. Pipelining does not weaken the consistency at all; it stops the consistency being paid for in *caller-visible latency*, which is a different resource entirely. What it does cost is crash exposure: with eight in flight, a primary that dies can leave eight commands unacknowledged instead of one.

So the honest model is one dial with three coupled quantities: how many commands may be in flight, how much latency the caller sees, and how much a crash can lose. Synchronous-with-window-1 and asynchronous are the two ends of it, and almost everything interesting lives in between. I would not have found that by reasoning; I found it by building both and measuring.

## The tradeoffs

**Every command must be replayable.** Nothing may enter the engine except through the log. A single code path that mutates state directly, an administrative override, a debug hook, breaks the replica silently and permanently.

**Slow backups become the primary's problem.** Under synchronous replication the primary goes no faster than its slowest backup. Under asynchronous, the backlog accumulates somewhere, and if that somewhere is unbounded memory then a slow backup kills the primary. Flow control is not optional here; it is the difference between degraded and dead.

**Consistency costs throughput, measurably.** Roughly 1.9x in this system. That is a real price, not a rounding error, and pretending otherwise is how people end up surprised.

**Requiring every backup to acknowledge is stricter than a quorum.** Sententia commits when all backups have a command, which is simple to reason about and means one stuck backup stalls progress. A majority quorum tolerates a slow minority, and needs the same notion of a majority that elections do, so both arrive together.

**The log grows forever unless something trims it,** and trimming is what creates the case where a backup falls too far behind to catch up by replay at all. That is what snapshots are for, and not having them yet is a real gap rather than a detail.

## How I used it in the project

Sententia's Phase 3: a `CommandLog`, a `Replicator`, and both acknowledgement modes, over the Phase 2 framed TCP transport.

The pieces that made it work were mostly built earlier for other reasons, which was the pleasant part.

The `Command` type carries no engine-assigned fields, decided in Phase 1 so a command would be a complete description of an input. That made forwarding one a straight encode with nothing to reconcile.

The state checksum walks the book in canonical order and is a function of state rather than history, asserted in Phase 1 by building the same book two different ways and requiring equal hashes. That is what lets a backup that caught up by bulk replay compare equal to a primary that got there live.

Best of all, catch-up correctness was already proven. Phase 1 had a prefix-replay test: apply a third of a sequence, then the rest, and require the result to equal applying it all at once. I wrote it as a down payment on Phase 5 recovery. It turned out to be exactly the property catch-up needs, three phases early.

The end-to-end confirmation is one number. Replaying the same order file through the single-process driver, through a synchronous pair, and through an asynchronous pair gives the same state checksum, `17441047841503333197`, on all three.

## What surprised me

**A correctness test cannot tell a good implementation from a catastrophic one.** This is the lesson of the phase and I will not forget it.

A use-after-move left the cursor tracking "how far have I sent this backup" permanently at zero. Moving the message into the variant emptied its entries vector, and the bookkeeping that read it afterwards saw nothing. So every submit re-sent the entire outstanding range from the beginning.

Replicating 1,000 commands cost 883,003 messages and 91 MB, instead of 1,001 messages and 72 KB. Roughly 880 times the messages.

**Every correctness test passed.** All of them. Including the chaos test that severs the connection hundreds of times. Re-sending changes no state: the backup detects the overlap, applies what is new, and converges to precisely the right checksum. The system was provably correct and completely unusable, and the only reason I found it is that the benchmark stopped finishing.

I now think any system with a cost dimension needs a test that asserts the cost. Not a benchmark you run occasionally and eyeball, an assertion that fails. Sententia has one that requires messages and bytes to stay proportional to the command count, and the benchmark prints bytes-per-command next to throughput so amplification shows up in the headline.

**I checked that the convergence tests could actually fail.** After they all passed on the first run I got suspicious, sabotaged the backup to skip one command in every five hundred, and re-ran. Six assertions failed with visibly different checksums; restoring the code gave a clean pass. This is the same habit as Phase 1's sensitivity test and it keeps earning its keep. A convergence test that cannot detect divergence is decoration.

**The unbounded queue was invisible until the phase that used it.** Phase 2's transport had no cap on outbound buffering. With heartbeats as the only traffic it was fine, and I shipped it. Replication is the first thing that writes continuously to a peer that might stall, and a peer that stopped reading grew the sender to 65 MB and climbing while bytes actually written flatlined at 4.26 MB.

The general shape: the phase that introduces a new load pattern is where the previous phase's unbounded thing finally bites. Worth asking, at the end of every phase, what in it is unbounded and what would have to happen for that to matter.

## References

- Schneider, *Implementing Fault-Tolerant Services Using the State Machine Approach: A Tutorial* (1990). The canonical statement, and unusually explicit that determinism is a requirement rather than an assumption.
- Ongaro and Ousterhout, *In Search of an Understandable Consensus Algorithm* (2014). The `AppendEntries` and gap-detection shape here is deliberately Raft-like, minus the elections.
- Gray and Lamport, *Consensus on Transaction Commit* (2006), on why the synchronous round trip is genuinely unavoidable when you want the guarantee.
- Kleppmann, *Designing Data-Intensive Applications*, ch. 5. The clearest treatment of the synchronous versus asynchronous replication tradeoff and what each one actually loses.
- [`./determinism-and-replication.md`](./determinism-and-replication.md) - the Phase 1 entry, which is the precondition for everything here.
- [`./tcp-framing-and-partial-io.md`](./tcp-framing-and-partial-io.md) - the Phase 2 transport this runs on.
