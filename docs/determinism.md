# Determinism: The Phase 1 Contract

This is the document the rest of the project depends on. Everything in
Phases 3 through 5 assumes the property described here, and none of it
works if the property is not true.

## The claim

> Given the same starting state and the same ordered sequence of
> commands, `MatchingEngine` emits exactly the same events, in exactly
> the same order, and ends in exactly the same state. Every run. Every
> process. Every platform.

## Why it is the precondition for everything else

Phase 3 replicates order state from a primary node to a backup. There are
two ways to do that:

1. **Ship the state.** After every command, send the resulting book to
   the backup. Simple, correct, and unusably expensive: the book is far
   larger than the command that changed it, and the cost per command
   grows with the size of the book rather than with the size of the
   change.

2. **Ship the commands.** Send the backup the same commands the primary
   received, and let it compute the same state itself. The message is
   tiny and constant-sized regardless of how large the book gets.

Option 2 is what real systems do, and it is only correct if the two nodes
compute the *same* state from the same input. That is exactly the
determinism claim. Without it, the backup silently drifts from the
primary, and the drift is not detected until a failover promotes a
replica whose book disagrees with the one it replaced, at which point the
system is producing wrong trades and there is no record of when it
started.

The same reasoning drives Phase 5 recovery. A node that crashes and
rejoins rebuilds its state by replaying the log from a checkpoint. If
replay is not deterministic, the recovered node is not the node that
crashed, and consensus on state becomes impossible to establish.

So: determinism is not a nice property of this engine. It is the
substrate. This is why the phase plan puts the determinism test before
any distribution work, and why a red determinism test means stopping
rather than continuing.

## Sources of non-determinism, and what was done about each

### Wall-clock time

**Excluded from the core entirely.** The engine has no access to a clock.
Time priority is established by `arrivalCounter_`, an integer the engine
increments itself when an order comes to rest.

This is the single most important design move in the phase. A timestamp
taken inside the engine would differ between the primary and the backup
by whatever the two machines' clocks disagree by, which is never zero.
Ordering by an engine-assigned counter means the ordering is a function
of the input sequence and nothing else.

Timing still exists; it just lives at the edges. `bench/engine_bench.cpp`
calls `steady_clock` around `apply`, from outside. The engine does not
know it is being timed, and the measurement cannot influence the result.

### Randomness

**Excluded from the core entirely.** Nothing in `src/` consults a random
number generator.

The test suite does use one, deliberately, and it lives in `tests/`. The
generator is a hand-written LCG rather than `std::mt19937` plus
`std::uniform_int_distribution`, because the standard library's
*distributions* are not specified to produce the same values across
implementations. A cross-platform determinism test built on
`uniform_int_distribution` would feed two platforms two different command
sequences and then report the resulting difference as an engine bug. The
LCG is fully specified by its own arithmetic, so every platform generates
the same stream.

### Container iteration order

**The subtle one.** Hash containers iterate in an order that depends on
the hash function, the bucket count, the insertion history, and the
allocator. None of those are guaranteed stable across standard library
implementations, and some are not stable across runs of the same binary.

Every container whose iteration order can reach the output is ordered:

| Structure | Container | Why |
|-----------|-----------|-----|
| Bid levels | `std::map<Price, OrderQueue, std::greater<>>` | Iteration order is price order, by construction. |
| Ask levels | `std::map<Price, OrderQueue, std::less<>>` | Same, ascending. |
| Orders in a level | `std::list<RestingOrder>` | FIFO, with stable iterators so cancel is O(1). |
| Order id index | `std::unordered_map<OrderId, Locator>` | **Never iterated.** Point lookups only. |

That last row is a standing invariant rather than something the type
system enforces. Iterating the id index would reintroduce exactly the
non-determinism the other three rows exist to prevent. If a future change
needs to walk every live order, it must walk the levels, not the index.

### Floating point

**Not used.** Prices are integer ticks. See `docs/domain-model.md`.

### Threads

**None in the core.** `MatchingEngine` is single-threaded by contract.
There are no atomics, no locks, and no shared mutable state. Concurrency
in this project belongs to the transport in Phase 2 and above; the
matching core stays a plain state machine, and stays testable as one.

### Uninitialised memory

Every field of every domain type has a default member initialiser, so no
struct can carry indeterminate bytes into a checksum.

## How the claim is tested

`tests/test_determinism.cpp` makes four separate arguments. Each one
covers a failure the others would miss.

### 1. The golden stream

A short hand-written command sequence with its complete expected event
stream written out line by line, all twenty-two events. This is the only
test that checks the engine agrees with what a *person* decided it should
do; every other test here only checks the engine agrees with itself.

It also makes the text rendering part of the contract. Two runs that
produce the same events produce byte-identical text, which is what lets
the replay driver's output be diffed between nodes.

### 2. Repeated runs

Twenty thousand generated commands, applied to fifty freshly constructed
engines in one process. Every run must produce an identical event hash
and an identical state checksum, and one run is additionally compared
event by event rather than by hash, so a hash collision cannot hide a
difference.

Fifty runs in one process is aimed squarely at the failures that only
appear on the second run: allocator reuse handing back a different
address and changing a hash bucket, or state surviving in a static that
should have been per-engine.

### 3. Prefix replay

Apply the first third of a sequence, record the state checksum, then
apply the rest. The result must equal applying the whole sequence in one
go, and a second engine driven over just the prefix must reach the same
midpoint.

This is the operation a recovering node performs in Phase 5, tested three
phases before anything needs it. It establishes that state after N
commands is a pure function of those N commands, with no dependence on
how the application was chunked.

### 4. Sensitivity

The tests above would all pass on an engine that ignored its input and
emitted nothing. So: perturb one quantity by one unit and require the
output hash to change, then swap two adjacent commands and require the
output to change again.

The second half is the interesting one. It establishes that the *order*
of the log matters, not just its contents, which is precisely why Phase 4
needs a leader to impose a total order rather than letting nodes accept
commands independently.

### Beyond the determinism test

`tests/test_invariants.cpp` drives forty independently seeded command
sequences and checks structural properties after every single command:
levels sorted, no empty levels, no zero-quantity orders, arrival ordinals
ascending within a level, the id index agreeing with the levels about how
many orders exist, the book never crossed, event sequence numbers gapless
and ascending, and quantity conserved across the whole run. That last one
is the strongest: everything accepted must equal everything traded, plus
everything resting, plus everything cancelled.

`scripts/replay_determinism.cmake` runs the replay binary twice over the
same file and fails on any byte of difference, which exercises the claim
through the real I/O path rather than in process.

CI extends the same check across GCC, Clang, MSVC and Apple Clang, then
compares the checksums the four platforms produce and fails if any two
disagree. A determinism property that only holds on one toolchain is not
the property this project needs.

## Checksums

`OrderBook::checksum()` is FNV-1a over the full book walked in canonical
order: bids by descending price, asks by ascending price, orders in FIFO
order within each level. `MatchingEngine::stateChecksum()` combines that
with the engine's counters.

The point of the canonical walk is that the checksum is a function of
*state*, not of history. Two books that arrived at the same contents by
different routes hash to the same value, which is what makes the
checksum a usable "are these two replicas in agreement" test later. It is
asserted directly in `test_order_book.cpp`.

FNV-1a is not cryptographic and is not meant to be. It defends against
accidental divergence between cooperating nodes, not against an adversary
constructing a collision.

## What this does not claim

- **Not thread safety.** The core is deterministic *because* it is
  single-threaded, not in spite of it.
- **Not timing determinism.** Two runs take different amounts of wall
  time. The benchmark shows this directly: across repeated runs the
  latency percentiles move around while the state checksum does not.
  That contrast is the property, stated as a measurement.
- **Not determinism of anything outside `src/`.** Parsing, printing,
  argument handling and timing all live at the edges precisely so that
  the core can make a claim this strong.
