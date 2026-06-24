# Phase 1 Completion Report: Single-Node Baseline

Checked against the must-haves, should-haves and definition of done in
`02_PHASE1_SINGLE_NODE.md`.

## Must-haves

| Requirement | Status | Where |
|-------------|--------|-------|
| Clean command/event model, documented | Done | `include/sententia/command.hpp`, `event.hpp`, `docs/domain-model.md` |
| Matching core separated from I/O; core pure and deterministic | Done | `src/` has no I/O; all of it is in `apps/replay/` and `bench/` |
| Determinism test: fixed input, identical output every run | Done | `tests/test_determinism.cpp`, four independent arguments |
| Solid unit-test coverage of matching and edge cases | Done | 9 test binaries, 10 ctest entries |
| No wall-clock, randomness, or threading inside the core | Done | Verified by inspection, see below |

The last one is checkable mechanically:

```
$ grep -rnE '#include <(chrono|random|thread|mutex|atomic|ctime)>' src/ include/
(no matches)
```

The core cannot reach a clock, an RNG, or a thread, because it does not
include the headers that provide them. The only matches for those words
anywhere under `src/` and `include/` are in comments explaining their
absence.

## Should-haves

| Requirement | Status | Where |
|-------------|--------|-------|
| Property-based testing of invariants | Done | `tests/test_invariants.cpp`, 40 seeds, ~224k assertions |
| CLI driver that feeds orders from a file and prints events | Done | `apps/replay/` |
| Micro-benchmark of the single-node core | Done | `bench/engine_bench.cpp` |

## Definition of done

> You have a single-node matching engine whose core is pure,
> deterministic, and well-tested. The same command sequence always
> produces the same events and final state, provably, via a test.

Met. `ctest` runs 10 tests, all green. The determinism test alone makes
183 assertions across 50 repeated runs of a 20,000-command sequence, a
prefix-replay equivalence check, a byte-exact golden stream, and two
sensitivity checks that rule out a trivially-passing engine.

## Baseline numbers

For comparison against the distributed engine in Phase 6. One million
commands after a 100,000-command warmup, single thread, `-O3`, GCC 13.3.

```
commands=1000000
events=2723044
trades=689931
resting_orders=76871
throughput_cmds_per_sec=2336969
latency_ns: p50=179  p95=677  p99=8026  p999=10840  max=2684520
state_checksum=6588492516400874185
```

Measured in a shared cloud container, so treat these as an order of
magnitude rather than a hardware number. The tail is dominated by
allocation: `std::map` nodes and `std::list` nodes are heap-allocated per
price level and per order, so p99 reflects allocator behaviour more than
matching logic. Replacing them with pool-allocated intrusive structures
is Celeritas Phase 5 work and is deliberately not done here, because
Phase 1's job is correctness and determinism, not speed.

The useful observation from this table is the contrast in the repeated
runs recorded in `docs/determinism.md`: the latency percentiles move
between runs while `state_checksum` does not change at all. That is the
separation of concerns this phase was built to establish, expressed as a
measurement rather than an assertion.

## Notable decisions

**A minimal test harness instead of GoogleTest.** Phase 0 suggested
GoogleTest as the safe choice. The suite here uses roughly sixty lines of
hand-written harness instead, because fetching a framework at configure
time makes the build depend on the network and on an external version,
and this project's whole thesis is reproducibility. The harness gives
`CHECK`, `CHECK_EQ`, `CHECK_STR_EQ` and a pass/fail summary, which is
everything the matching tests actually need. If a later phase needs
fixtures or parameterised tests, revisit it then.

**One executable per test.** Each test file links its own binary and gets
its own ctest entry, so a crash isolates to one named test rather than
taking the suite with it. This matches the Celeritas layout.

**Warnings as errors, four toolchains.** `-Wall -Wextra -Wpedantic
-Wshadow -Wconversion -Wsign-conversion -Werror`, built on GCC, Clang,
MSVC and Apple Clang. `-Wconversion` in particular caught real
sign-conversion mistakes during development.

**Cross-platform determinism in CI.** CI does not just run the tests on
each platform; it replays the same order file on Linux, Windows and macOS
and fails if the resulting checksums differ. A determinism property that
holds only on the machine it was developed on is not the property Phase 3
needs.

## What this sets up

- **Phase 2 (networking)** carries `Command` over the wire. Because
  commands contain no engine-assigned fields, serialisation is a
  straight struct encode with nothing to reconcile.
- **Phase 3 (replication)** ships commands rather than state, which is
  only correct because of the determinism contract. `stateChecksum()` is
  the primary/backup agreement check.
- **Phase 4 (leader election)** exists to impose a total order on the
  command log. The sensitivity test showing that swapping two adjacent
  commands changes the output is exactly why that ordering has to be
  agreed rather than assumed.
- **Phase 5 (recovery)** replays a log from a checkpoint. The prefix
  replay test already establishes that this produces the same state as
  continuous operation.

## Known gaps, carried forward deliberately

- **No persistence.** State is in memory only. A log and checkpoint
  format is Phase 3 and Phase 5 work, and designing it now would be
  guessing at requirements the networking layer has not produced yet.
- **Allocation in the hot path.** Every resting order allocates a list
  node and possibly a map node. Known, measured, and left alone: fixing
  it is a performance exercise that would complicate the code before the
  distributed behaviour that justifies it exists.
- **One instrument per engine.** See `docs/domain-model.md`.
- **No `docs/celeritas_baseline.md`.** That is a Phase 0 artifact about
  re-reading the existing Celeritas source. This repository is the
  Phase 1 deliverable and does not reproduce it.
