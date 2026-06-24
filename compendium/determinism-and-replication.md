# Determinism as the Precondition for Replication

> You cannot keep two machines in agreement by copying their memory. You keep them in agreement by feeding them the same instructions and trusting them to compute the same answer. That trust is a property you have to engineer, and it is called determinism.

**Learned during:** Sententia Phase 1, single-node baseline
**Tags:** `distributed-systems` `replication` `event-sourcing` `state-machine-replication` `determinism`

---

## The problem it solves

Two machines, one order book. The primary receives orders from the market and matches them. The backup needs to hold an identical book, so that if the primary dies mid-session the backup can take over without anybody noticing.

The obvious approach: after every order, the primary sends the backup its current book.

This fails on cost, and it fails badly. A single order is maybe forty bytes. A book with eighty thousand resting orders is several megabytes. You would be shipping megabytes to describe a forty-byte change, and the cost per order would scale with the size of the book rather than with the size of the change. At the point where the venue is busiest, and the book is deepest, replication would be at its most expensive. That is exactly backwards.

You could ship a diff instead of the whole book. Now you are maintaining a diff format, and the diff of a matching operation that sweeps four price levels is not obviously smaller than the four events describing it. You are also still shipping *consequences* rather than *causes*, which means the backup is a passive recipient that cannot verify anything.

The approach that actually works is to send the backup the same orders the primary received and let it do the matching itself. The message stays forty bytes no matter how deep the book gets. But it only works if the backup, given the same orders, produces the same book.

That "if" is the entire subject of this note. It sounds like it should be free. It is not.

## The core idea

Split the system into two kinds of message.

A **command** is an intent arriving from outside. "Buy 100 at 4250." It is a request. It might be rejected. The engine has not looked at it yet.

An **event** is a fact the engine has already decided. "Order 7 traded 40 units against order 3 at 4250." It is not a request and it cannot fail. It happened.

The engine is then a function:

```
apply(command) -> [events]
```

with all its state on the inside. Feed it a sequence of commands, get a sequence of events, and the state is whatever those events left behind.

Once the engine has that shape, replication becomes a shipping problem rather than a synchronisation problem. Send the commands, in order, to every node. Each node applies them independently. They all arrive at the same state without ever comparing states, because they all ran the same function over the same input.

This is state machine replication, and it is the pattern underneath Raft, underneath Kafka's log, underneath every database that replicates by shipping its write-ahead log. It has one requirement: the state machine has to be deterministic.

## How it works

The mechanism is the easy part. Here is the whole thing.

```
             commands (ordered, identical)
                    |
        +-----------+-----------+
        |                       |
        v                       v
   +---------+             +---------+
   | PRIMARY |             | BACKUP  |
   |         |             |         |
   | apply() |             | apply() |
   +---------+             +---------+
        |                       |
        v                       v
    state S                 state S
        |                       |
        +--- checksum(S) == checksum(S) ---+
                     agreement
```

Both nodes run the identical `apply`. Neither node ever sends the other its state. The only thing crossing the wire is the command stream, and the only thing needed to verify agreement is a cheap hash of the resulting state.

Recovery falls out of the same structure for free. A node that crashes and restarts holds a checkpoint (state as of command N) and a log (commands N+1 onward). It loads the checkpoint, replays the log, and it is current. This is only valid if replaying a prefix and then continuing gives the same result as never having stopped, which is the same determinism property viewed from a different angle.

And the reason to build the engine this way *before* there is a second node: the shape is not something you can retrofit. If matching logic and socket handling are tangled together, there is no `apply` to run on the backup. The split has to exist first.

## The key insight

Here is the part I had backwards when I started.

I assumed the hard problem in replication was the network: dropped messages, reordering, partitions, timeouts. Those problems are real and they are where the famous algorithms live. But they are all about getting the *same command sequence* to every node. Consensus solves that. Raft solves that.

Consensus does not solve, and does not even address, the question of whether two nodes given identical input produce identical output. It assumes it. Every consensus paper assumes a deterministic state machine, usually in a sentence you skim past on the way to the interesting part.

So determinism is not one of the difficulties of replication. It is the *assumption underneath* the difficulties. If your state machine is not deterministic, running Raft in front of it does not help at all. You get perfect agreement on the command log and divergent state anyway.

The second, sharper insight is what actually breaks determinism in practice. It is not exotic. It is a short list of very ordinary things:

**Wall-clock time.** Timestamping an order with `now()` inside the engine means the primary and the backup stamp it differently, because no two clocks agree. If that timestamp then affects ordering, the books diverge. The fix is to stop calling it time. What matching actually needs is *ordering*, not time, and ordering can be a counter the engine increments itself. A counter is a function of the input sequence. A clock is not.

**Hash container iteration order.** This is the one that bites quietly. `std::unordered_map` iterates in an order determined by the hash function, the bucket count, the insertion history, and the allocator. None of that is stable across standard library implementations, and some of it is not stable across runs of the same binary. If any iteration over a hash container can reach the output, you have non-determinism, and it will present as a rare divergence under production load rather than as a test failure.

**Floating point.** Deterministic on a fixed platform with fixed flags. Not portably so. A primary compiled with GCC and a backup compiled with MSVC can disagree in the last bit, and in a matching engine the last bit is money. Integer ticks, always.

**Uninitialised memory.** A struct with an indeterminate padding byte hashes differently on two machines even when every field it actually uses is identical.

**Threads.** Two threads racing over the book produce an interleaving that depends on the scheduler. The scheduler is not part of the command log.

What all five have in common: each is a place where the engine consults something that is not its input. Determinism is exactly the discipline of never doing that. The engine may read its commands and its own state. Nothing else.

Which gives a clean architectural rule. Clocks, RNGs, sockets, files, and threads are not banned from the system. They are banned from the *core*, and pushed to the edges. In Sententia the benchmark still measures latency with `steady_clock`, but it calls it from outside `apply`. The engine does not know it is being timed and the measurement cannot influence the result. Timing became an observation made from outside a core that has no notion of real time, and that turned out to be a much cleaner design than the one I would have written without the constraint.

## The tradeoffs

**You give up wall-clock timestamps inside the engine.** If a regulator wants "what time did this trade happen", the engine cannot answer, because it does not know. Something at the edge has to stamp commands on arrival, before they enter the log, and then that timestamp is part of the input and is replicated like anything else. That is the correct design, but it does mean the timestamp is assigned by whichever node received the order, not by the engine, and you have to be honest about what it means.

**You give up threading the core.** Sententia's matching engine is single-threaded by contract, and that is a real ceiling on single-node throughput. The escape is to parallelise *across* instruments (one deterministic engine per book, many books in a process) rather than inside one book. That works, but it is a constraint imposed by the replication story rather than one chosen on its merits.

**You give up convenient data structures.** Ordered maps instead of hash maps in every position whose iteration can reach the output. Ordered maps are slower and touch more cache lines. In Sententia the id index is still a hash map, which is safe only because it is never iterated, and that safety is a standing invariant maintained by discipline rather than by the type system. Determinism costs a little performance and a little vigilance, permanently.

**You give up cheap idempotence.** Because state is a function of the whole ordered command history, replaying a command twice is not a no-op. The order of the log matters as much as its contents, which is exactly why Phase 4 needs a leader to impose a total order rather than letting nodes accept orders independently.

**What you get:** replication messages that stay small no matter how large the state grows, recovery that is just replay, and a cheap agreement check between nodes (compare a hash, not a book). Debugging also gets dramatically better, which I did not anticipate. Every bug is reproducible by definition. There is no "it only happens under load."

## How I used it in the project

Sententia Phase 1 is a single-node matching engine built to be worthy of distribution later. Command and event are separate types. `MatchingEngine::apply(Command) -> [Event]` is the whole interface. `src/` contains no clock, no RNG, no thread, and no I/O, verifiable by the fact that it does not include the headers that provide them. Parsing, printing and timing live in `apps/` and `bench/`, outside the line.

Time priority is an `arrivalCounter_` the engine increments itself. Price levels are `std::map` with explicit comparators, so iteration order is price order by construction. Orders within a level are `std::list`, giving FIFO plus stable iterators so a cancel is O(1) and does not disturb anyone else's queue position. Prices are `int64_t` ticks.

The interesting work was in testing the claim, because a determinism test is unusually easy to write badly. Mine makes four separate arguments:

**A golden stream.** A short hand-written command sequence with all twenty-two of its expected events written out line by line. This is the only test that checks the engine agrees with what a *person* decided it should do. Every other test here only checks that the engine agrees with itself, which a broken engine can also do.

**Fifty repeated runs** of a twenty-thousand-command sequence in one process, comparing event hashes and state checksums. Repeating in one process specifically targets the failures that only appear on the second run: an allocator handing back a different address and changing a hash bucket, or state surviving in a static that should have been per-engine.

**Prefix replay.** Apply a third of the sequence, record the checksum, apply the rest, and require the result to equal applying the whole thing in one go. This is precisely what a recovering node does, tested four phases before anything needs it.

**Sensitivity.** All three tests above would pass on an engine that ignored its input and emitted nothing. So perturb one quantity by one unit and require the output to change, then swap two adjacent commands and require the output to change again. The second half is the one worth having: it demonstrates that the *order* of the log matters and not just its contents, which is the concrete reason a leader is needed later.

CI does not just run these on Linux. It replays the same order file on GCC, Clang, MSVC and Apple Clang and fails if any two platforms produce different checksums. A determinism property that holds only on the machine it was developed on is not the property replication needs.

## What surprised me

**The sensitivity test was an afterthought and it should not have been.** I had the repeated-run test passing and felt good about it, then realised that an engine which threw away every command and emitted nothing would pass it perfectly. A determinism test proves outputs are *stable*. It says nothing about whether they are *correct*, or whether the input is being read at all. Stability and correctness are independent properties and I had conflated them. The golden stream and the sensitivity checks exist because of that realisation, and I now think any invariant test needs a paired test that it fails on a deliberately broken implementation.

**The hash map iteration trap is much closer to the surface than it looks.** My first order index was an `unordered_map`, and my first instinct for "walk every live order" was to iterate it. That would have been a correctness bug invisible to every test I had, on the machine I was developing on, in a codebase where nothing else looked wrong. It would have shown up as a rare production divergence between two nodes months later. Discovering that the dangerous construct is *iteration*, not the container itself, was the moment the abstract rule turned into something I actually apply while typing.

**Removing the clock made the design better, not worse.** I expected "no wall-clock time in the core" to be a cost paid for replication. It turned out to be a clarification. Matching never wanted time, it wanted ordering, and I had been conflating those because a timestamp is the ordering mechanism you reach for by default. Once the engine's only notion of time was a counter, time priority became trivially correct and trivially testable. A constraint accepted for one reason paid off for an unrelated one, which is usually the sign that the constraint was pointing at something real.

**The benchmark ended up demonstrating the property rather than just measuring speed.** Across repeated runs the latency percentiles move around by a wide margin while the state checksum does not change at all. That contrast is the entire architecture in one output block: computation is deterministic, timing is not, and timing is an observation made from outside. I did not design the benchmark to show that. It just fell out, and it is now the thing I would point at first if someone asked me to explain what this phase was for.

## References

- Schneider, *Implementing Fault-Tolerant Services Using the State Machine Approach: A Tutorial* (1990). The original statement of the pattern, and the paper that spells out the determinism requirement explicitly instead of assuming it.
- Ongaro and Ousterhout, *In Search of an Understandable Consensus Algorithm* (Raft, 2014). Section 2 is where the deterministic state machine is assumed in a single sentence.
- Fowler, *Event Sourcing* (martinfowler.com). The same command and event split arrived at from the application-architecture direction rather than the distributed-systems one.
- [`../papers/lmax-disruptor.md`](../papers/lmax-disruptor.md). LMAX built a deterministic single-threaded business core for exactly these reasons, and got the replication and the performance from the same decision.
- [`../project-retrospectives/celeritas.md`](../project-retrospectives/celeritas.md). The single-process matching engine this work extends.
