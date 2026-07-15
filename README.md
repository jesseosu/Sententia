# Sententia

**A fault-tolerant distributed order matching engine in C++20.**

Multiple matching nodes with leader election, replicated order state, and
deterministic recovery from node failure, extending the single-process
[Celeritas](https://github.com/jesseosu/celeritas) engine into a
clustered, crash-resilient system.

**Status: Phase 2 complete.** The single-node core is pure and
deterministic, and nodes can now talk to each other over TCP. Replication
starts in Phase 3.

---

## What exists today

**A deterministic matching engine** (Phase 1) and **a message transport
that connects nodes over TCP** (Phase 2). They are not wired together
yet: that is Phase 3's job, and doing it earlier would mean guessing at a
replication design before the transport under it had been proven.

### The engine

A single-instrument limit order book with price-time priority matching,
built as a pure state machine:

```
Command  ---->  MatchingEngine::apply  ---->  [Event]
                        |
                        v
                   OrderBook state
```

The engine has no clock, no randomness, no threads, and no I/O. It is a
function from a command sequence to an event sequence. That is not an
aesthetic preference: it is the property the entire rest of the project
is built on, and Phase 1 exists to establish it before anything depends
on it.

### The determinism contract

> Given the same starting state and the same ordered sequence of
> commands, the engine emits exactly the same events, in exactly the same
> order, and ends in exactly the same state. Every run. Every process.
> Every platform.

Phase 3 replicates a primary node's order state to a backup by shipping
*commands*, not state, because a command is small and constant-sized
while a book is neither. That only works if both nodes compute the same
state from the same input. Phase 5 recovers a crashed node by replaying
its log, which only works for the same reason.

So determinism is not a nice property here. It is the substrate, and a
red determinism test means stopping rather than continuing.
[`docs/determinism.md`](docs/determinism.md) is the full treatment:
every source of non-determinism, what was done about it, and how the
claim is tested.

### The transport

Length-framed binary messages over TCP, with `poll()` multiplexing,
static cluster membership from a config file, and graceful handling of
peers that disappear.

```
 offset  size  field     notes
 0       4     magic     0x544E4553, "SENT"
 4       2     version   protocol version
 6       2     type      MessageType
 8       4     length    payload byte count
 12      ...   payload
```

The length prefix is there because **TCP is a byte stream, not a message
stream**. One `write()` of 100 bytes can arrive as 30 then 70, or
coalesced with the next message. TCP preserves byte order and delivery;
it does not preserve message boundaries, because those were never
transmitted. So they have to be encoded in the data.

The design decision that mattered: `FrameReader` and `FrameWriter`
contain **no I/O at all**. They are pure byte-level state machines that
do not know what a socket is. That is the Phase 1 pure-core discipline
one layer up, and it means the pathological splits real networks produce
rarely and unrepeatably can be tested deterministically, one byte at a
time, with no sockets involved. Four of the five network test binaries
never open one.

[`docs/wire-protocol.md`](docs/wire-protocol.md) covers the format, the
untrusted-length defence, the duplicate-connection tiebreak, and why
`poll()` rather than `epoll()`.

## Build and run

Requires CMake 3.16+ and a C++20 compiler. No third-party dependencies,
and nothing is fetched at configure time. The networking layer is POSIX
only; the engine core builds and is tested everywhere.

```bash
make build     # configure and compile
make test      # run the full ctest suite
make bench     # single-node baseline benchmark
make replay    # run the sample order file through the replay driver
make cluster   # launch a local 3-node cluster
```

Or directly:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

On Windows with MSVC:

```
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build --build-config Release --output-on-failure
```

## The replay driver

`replay` reads a command file, applies it to a fresh engine, and prints
the event stream and the resulting checksums. It is the I/O edge: all
parsing and printing lives here so that none of it can leak into the
core.

```bash
./build/replay scripts/sample_orders.txt
```

```
seq=1 cmd=1 ACCEPTED id=1 side=BUY type=LIMIT tif=GTC px=100 qty=10
seq=2 cmd=1 RESTING id=1 side=BUY px=100 qty=10
seq=3 cmd=1 TOB bid=100x10 ask=none
...
seq=15 cmd=6 TRADE aggressor=6 resting=1 side=SELL px=100 qty=10
seq=16 cmd=6 TRADE aggressor=6 resting=2 side=SELL px=100 qty=2
seq=17 cmd=6 TOB bid=100x3 ask=102x8
...
commands=12
events=34
event_hash=9684792522070214564
book_checksum=11753875096244137027
state_checksum=17373410596180621386
```

Because the output is byte-stable for a given input, the determinism
property has a one-line demonstration:

```bash
diff <(./build/replay scripts/sample_orders.txt) \
     <(./build/replay scripts/sample_orders.txt)   # always empty
```

Diffing two *nodes'* output is how divergence gets caught in the later
phases.

Input format, one command per line, `#` starts a comment:

```
N <id> <BUY|SELL> <LIMIT|MARKET> <GTC|IOC> <price> <qty>
C <id>
```

## Matching rules

Price priority, then time priority, and nothing else. Not quantity, not
order id, not the submitter.

- Highest bid and lowest ask match first.
- Within a price level, earliest arrival matches first, tracked by an
  engine-assigned ordinal rather than by any clock.
- Execution is at the *resting* order's price, so price improvement goes
  to the aggressor.
- A partially filled resting order keeps its place at the front of its
  level.
- Cancelling and re-entering loses priority.

Full details, including the event ordering contract and the validation
order, are in [`docs/domain-model.md`](docs/domain-model.md).

## Tests

```
$ ctest --test-dir build --output-on-failure
100% tests passed, 0 tests failed out of 16
```

| Test | Validates |
|------|-----------|
| `test_order_book` | Resting, cancelling, reducing, level bookkeeping, top of book, checksum equality |
| `test_matching_basic` | Crossing, full fills, event ordering, price improvement |
| `test_matching_partial` | Partial fills both ways, queue position after a partial, multi-level sweeps |
| `test_price_time_priority` | Price beats time, time beats quantity, priority lost on re-entry |
| `test_cancel` | Cancel acks, unknown-order rejects, cancels of partially filled orders, deep cancels |
| `test_order_types` | Market orders, IOC, engine-initiated cancels, market-with-GTC reject |
| `test_edge_cases` | Empty book, validation order, id reuse, self-cross, extreme quantities, crossed book |
| `test_determinism` | **The contract.** Golden stream, 50 repeated runs, prefix replay, sensitivity |
| `test_invariants` | Property-based: 40 seeds, structural invariants after every command, quantity conservation |
| `replay_sample` | End-to-end byte-identical replay through the real I/O path |
| `test_wire` | Exact byte layout, round trips, bounds checks, untrusted length caps |
| `test_message_codec` | Frame headers, round trips, truncation, trailing bytes, out-of-range enums |
| `test_framing` | **Every chunk size from 1 byte up**, coalesced reads, split headers and bodies, bad magic, bad version, oversized payloads, short writes at every step size |
| `test_cluster_config` | Config parsing, malformed lines, duplicate ids, port ranges |
| `test_transport` | Handshake, 4,000-message ordering, duplicate-connection tiebreak, real partial writes, peer death, garbage input, dialing nothing |
| `two_node_demo` | Two real node processes, end to end |

`test_invariants` alone makes about 224,000 assertions.

### CI

GitHub Actions builds and tests on GCC, Clang, MSVC and Apple Clang with
`-Werror`, checks formatting, and then does something more specific: it
replays the same order file on Linux, Windows and macOS and **fails if
the resulting checksums differ**. A determinism property that holds only
on the machine it was developed on is not the property Phase 3 needs.

## Baseline performance

The number the distributed engine gets compared against in Phase 6. One
million commands after a 100,000-command warmup, single thread, `-O3`,
GCC 13.3, measured in a shared cloud container.

```
throughput_cmds_per_sec=2336969
latency_ns: p50=179  p95=677  p99=8026  p999=10840  max=2684520
```

Treat these as an order of magnitude rather than a hardware number. The
tail is dominated by allocation: `std::map` and `std::list` nodes are
heap-allocated per price level and per order, so p99 reflects the
allocator more than the matching logic. Pool-allocated intrusive
structures are deliberately not done here, because Phase 1's job is
correctness and determinism, not speed.

The useful observation is the contrast across repeated runs: the latency
percentiles move while `state_checksum` does not change at all. Timing is
an observation made from outside a core that has no notion of real time,
and that shows up directly in the measurement.

## Layout

```
include/sententia/       engine headers: types, command, event, order_book, engine
include/sententia/net/   transport headers: wire, message, framing, socket, transport
src/                     the pure core. no I/O, no clock, no threads, no RNG
src/net/                 the transport. POSIX sockets and poll()
apps/replay/             command-file replay driver (the I/O edge)
apps/node/               cluster node binary
bench/                   single-node baseline benchmark (the timing edge)
tests/                   one executable per test, plus a ~60-line harness
scripts/                 sample orders, cluster configs, demo and determinism checks
docs/                    domain model, determinism contract, wire protocol, phase reports
```

The split between `src/` and everything else is the architecture. Any
clock, RNG, socket, or file handle belongs on the outside of that line.

## Documentation

- [`docs/determinism.md`](docs/determinism.md) - the contract, why it is
  the precondition for replication, every source of non-determinism and
  what was done about it, and how the claim is tested.
- [`docs/domain-model.md`](docs/domain-model.md) - commands, events,
  matching rules, order lifecycle, validation order, and the omissions
  that were decisions rather than oversights.
- [`docs/wire-protocol.md`](docs/wire-protocol.md) - the frame format,
  why TCP forces you to frame at all, the untrusted-length defence, the
  duplicate-connection tiebreak, and poll() versus epoll().
- [`docs/phase-1-report.md`](docs/phase-1-report.md) and
  [`docs/phase-2-report.md`](docs/phase-2-report.md) - each phase checked
  against its definition of done, with the bugs found and the gaps
  carried forward.

## Roadmap

| Phase | Deliverable | Status |
|-------|-------------|--------|
| 0 | Repo, build system, CI, test harness | Done |
| 1 | Single-node baseline: pure deterministic core, tests | **Done** |
| 2 | Networking layer: TCP message passing between nodes | **Done** |
| 3 | State replication, primary to backup | Next |
| 4 | Leader election | |
| 5 | Fault-tolerant recovery | |
| 6 | Benchmarking and hardening | |
| 7 | Documentation and writeup | |

Phases 3 to 5 are the actual distributed-systems content. Phases 0 to 2
build the platform it stands on.

The governing rule is **finish beats scope**: every phase ends with a
complete, working, demonstrable artifact. A complete 3-node engine with
leader election beats an ambitious 5-node design that is 60% done.

## Why this project exists

Celeritas and Vigil are single-process systems that demonstrate depth in
low-latency and systems programming. This one demonstrates the next tier:
consistency, replication, consensus, and what happens when a node dies
mid-trade. That is the domain trading infrastructure actually runs in.

## Stack

C++20 · CMake · GCC / Clang / MSVC / Apple Clang · POSIX sockets ·
poll() · GitHub Actions · zero third-party dependencies

## License

MIT. See [LICENSE](LICENSE).
