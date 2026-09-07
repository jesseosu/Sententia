# Sententia

**A fault-tolerant distributed order matching engine in C++20.**

Multiple matching nodes with leader election, replicated order state, and
deterministic recovery from node failure, extending the single-process
[Celeritas](https://github.com/jesseosu/celeritas) engine into a
clustered, crash-resilient system.

**Status: Phase 5 complete.** State survives power loss, a crashed node
recovers to exactly the state it was in, and a leader can die mid-stream
without losing anything a client was told succeeded. Benchmarking and
hardening are Phase 6.

---

## What exists today

**A deterministic matching engine** (Phase 1), **a framed TCP transport**
(Phase 2), **state-machine replication** on top of both (Phase 3),
**Raft-style leader election** deciding who does the replicating
(Phase 4), and **durability plus crash recovery** underneath all of it
(Phase 5).

The end-to-end proof that it works is a single number. Replaying the same
order file through the single-process driver, through a synchronous
primary/backup pair, and through an asynchronous one all produce the
state checksum `17441047841503333197`. Neither node ever sent the other a
book.

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

### Replication

A backup needs the same book as the primary. Shipping the book costs
megabytes per order and gets more expensive exactly when the venue is
busiest. So the primary ships **commands** instead, and the backup
computes the book itself. Measured: **72.6 bytes per command, flat,
regardless of book depth.**

That only works because the engine is deterministic, which is why Phase 1
came first and why its determinism test gates everything here. This is
state-machine replication, the idea underneath Raft and Paxos and every
database that replicates its write-ahead log.

Both acknowledgement modes are implemented, because the tradeoff is worth
being able to measure. 20,000 commands, two nodes over loopback:

| Mode | In flight | Throughput | Submit p50 |
|------|-----------|-----------|-----------|
| Synchronous | 1 | 8,183 cmd/s | 61,325 ns |
| Synchronous | 8 | 8,264 cmd/s | 2,447 ns |
| Synchronous | 64 | 8,277 cmd/s | 2,432 ns |
| Asynchronous | unbounded | 15,692 cmd/s | 2,787 ns |

Consistency costs about 1.9x throughput. The more interesting row is the
window: strict lockstep makes the caller wait 61 microseconds per
command, and allowing eight in flight drops that 25x while throughput
barely moves. Pipelining does not weaken the guarantee, it stops the
guarantee being paid for in caller-visible latency. What it costs is
crash exposure.

[`docs/replication.md`](docs/replication.md) covers the model, sequencing,
catch-up, backpressure, and divergence detection.

### Leader election

Through Phase 3 the primary was designated by hand. Now the cluster picks
one itself, notices when it dies, and replaces it.

```
      +-------------+   no heartbeat       +-------------+
      |  FOLLOWER   | -------------------> |  CANDIDATE  |
      +-------------+   for a while        +-------------+
             ^                                | |      |
             |  sees a higher term,           | |      | majority
             |  or a leader of its own term   | |      v
             |                                | |  +----------+
             +--------------------------------+ +->|  LEADER  |
                                                   +----------+
```

The no-split-brain argument is one sentence: **two majorities of the same
cluster must overlap in at least one node, and that node votes at most
once per term**, so two candidates cannot both win a term. Not a
heuristic, arithmetic.

**The invariant is per-term, not per-instant.** A leader cut off by a
partition keeps calling itself leader until it hears otherwise, so there
really are two leaders for a while. That is fine: they are in different
terms and the isolated one cannot reach a quorum, so it can commit
nothing. Getting that distinction right is most of understanding why the
protocol works.

The election is a **pure state machine**: no socket, no clock, no thread.
Time arrives as a parameter and actions come back as a list for the
caller to perform. That is what makes split-brain testable rather than
hoped for, because the whole cluster runs in one process on a virtual
clock over a bus that can partition and crash on command:

| | |
|---|---|
| Randomised fault schedules | 115 |
| Cluster sizes | 3, 5, 7 |
| Message loss | up to 20 percent |
| Fault injections | roughly 6,900 |
| Terms with two leaders | **0** |

And the test was checked against deliberately broken implementations.
Setting the majority to 1, or removing the one-vote-per-term rule, both
report split-brain immediately with named reproducible seeds.

[`docs/consensus.md`](docs/consensus.md) covers terms, quorums, why
randomised timeouts are load-bearing, and the election restriction.

### Durability and recovery

Three files per node: the term and vote, the command log, and a snapshot.
The rule that makes them worth anything is ordering, not format: a
command reaches disk and is **flushed before** it is treated as
committed. A crash then leaves everything a client was told about on
disk, and anything not on disk was never acknowledged.

Every log record carries a CRC, because a crash mid-write leaves a
half-record that reads back as garbage shaped like data. Recovery replays
until a record fails to verify, then truncates. A torn tail is the normal
outcome of a crash, not an error.

Recovery is **not a special code path**. It loads a snapshot and then runs
the ordinary apply loop, fed from disk instead of a socket. That only
works because the engine is deterministic, which is Phase 1 paying off
for the fourth time.

The durability dial, measured:

| Policy | fsyncs per 100 appends | A power cut loses |
|--------|------------------------|-------------------|
| `EveryWrite` | 100 | nothing acknowledged |
| `Batched` (N=10) | 10 | up to 9 acknowledged commands |
| `Never` | 0 | everything not yet flushed |

**The commit rule changed too, and it matters.** Phase 3 committed when
every backup held an entry, so one dead backup stalled the cluster.
Phase 5 commits on a **majority**, plus Raft's Figure 8 restriction that a
leader may only advance the commit point to an entry from its own term.

A consequence worth stating: **a two-node cluster tolerates zero
failures**, because a majority of two is two. `test_replication` asserts
this directly, and the old expectation it replaced was the unsafe one.

[`docs/recovery.md`](docs/recovery.md) covers write-ahead ordering, torn
writes, atomic file replacement, snapshots and the gap they create, log
matching with per-entry terms, and why truncating a follower's log is
safe.

```bash
./scripts/election_demo.sh ./build/node
```
```
node 3 elected leader for term 1
killed node 3
election demo OK
  first leader:  node 3, term 1
  after failure: node 2, term 2
  no term ever had two leaders
```

```bash
./scripts/replication_demo.sh ./build/node \
    scripts/sample_orders.txt sync
```
```
replication demo OK (mode=sync)
  commands applied on both nodes: 12
  state checksum on both nodes:   17441047841503333197
```

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
make election  # 3 nodes elect a leader, then the leader is killed
make replbench # synchronous vs asynchronous replication benchmark
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
event_hash=10118579028301334196
book_checksum=2780187635739378981
state_checksum=17441047841503333197
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
100% tests passed, 0 tests failed out of 27
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
| `test_backpressure` | Bounded outbound queue, refusal reported, recovery when the peer resumes |
| `test_command_log` | Sequencing, gap refusal, range serving, truncation |
| `test_replication` | Sync and async convergence, commit semantics, gap detection, **message amplification**, full catch-up from zero, compaction |
| `test_replication_chaos` | Convergence under repeated link failure, 4 seeds x 4 drop rates |
| `replication_demo_sync` / `_async` | Two real processes reaching identical checksums |
| `test_election` | States, terms, quorums, one vote per term, the election restriction, randomised timeouts |
| `test_election_sim` | **No split-brain** across 115 randomised partition and crash schedules on 3, 5 and 7 nodes |
| `election_demo` | Three real processes, leader killed, replacement elected in a higher term |
| `test_durability` | WAL round trips, **torn writes**, corrupted payloads, sync policies, snapshot round trips |
| `test_recovery` | Restart replays to the identical checksum, snapshot-bounded recovery, **crash between snapshot and truncation**, durable votes, leader crash losing nothing committed |

`test_invariants` alone makes about 224,000 assertions.

### Checking the tests themselves

A green suite proves the tests agree with the code, which is also true
when both are wrong, and true when a test cannot observe the thing it is
named after. This project has shipped two tests that passed while being
structurally incapable of failing. So there are three gates on the tests:

```bash
make mutants   # break the code 20 ways, assert a named test notices each
make flake     # run the suite 10 times, fail if any run differs
make asan      # ASan and UBSan, because assertions cannot see memory errors
```

`make mutants` runs a registry of 20 mutations, each attacking a property
the project actually claims. **Its first run found four invariants that
nothing was guarding**, including Raft's Figure 8 rule, which is the
subtlest correctness property here and was documented at length and
tested by nothing. It also found dead code, because a mutation landed on
a function nothing called any more.

```
$ make mutants
  killed   election-majority-of-one      by test_election_sim, test_election
  killed   recovery-double-applies-at-boundary   by test_recovery
  ...
killed 20  survived 0  broken 0
```

`make flake` exists because the most repeated mistake in this project,
four times across five phases, was bounding a test by something the
environment controls: a short write that only happens when kernel buffers
are small, a cycle budget that expires under load, a queue depth sampled
at one instant. A single green run cannot tell "correct" from "correct on
this machine when nothing else was happening".

[`docs/testing-standards.md`](docs/testing-standards.md) has the rules,
each one written because it was broken first.

### CI

GitHub Actions builds and tests on GCC, Clang, MSVC and Apple Clang with
`-Werror`, checks formatting, runs all three gates above, and then does
something more specific: it replays the same order file on Linux, Windows
and macOS and **fails if the resulting checksums differ**. A determinism
property that holds only on the machine it was developed on is not the
property Phase 3 needs.

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
include/sententia/replication/  command log and replicator
include/sententia/consensus/    leader election state machine
include/sententia/storage/      write-ahead log, stable store, snapshots
src/                     the pure core. no I/O, no clock, no threads, no RNG
src/net/                 the transport. POSIX sockets and poll()
src/replication/         state-machine replication
src/consensus/           leader election. no socket, no clock, no thread
src/storage/             durability. the only place that touches a disk
apps/replay/             command-file replay driver (the I/O edge)
apps/node/               cluster node binary: engine + log + transport + replicator
bench/                   single-node baseline benchmark (the timing edge)
tests/                   one executable per test, a ~60-line harness, and a
                         deterministic cluster simulator (sim.hpp)
scripts/                 sample orders, cluster configs, demos, and the
                         mutation and flake gates
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
- [`docs/replication.md`](docs/replication.md) - shipping commands rather
  than state, sequencing, the acknowledgement tradeoff with numbers,
  backpressure, catch-up, and divergence detection.
- [`docs/consensus.md`](docs/consensus.md) - terms, quorums, why
  randomised timeouts are load-bearing, the election restriction, and the
  per-term invariant.
- [`docs/recovery.md`](docs/recovery.md) - write-ahead ordering, torn
  writes, snapshots, per-entry terms, the majority commit rule and the
  Figure 8 restriction.
- [`docs/testing-standards.md`](docs/testing-standards.md) - how the
  tests are checked, and the rule behind each gate.
- [`docs/failure-modes.md`](docs/failure-modes.md) - a running catalogue
  of every bug and near-miss, with symptom, cause, fix, and what catches
  it now. Worth reading before the phase reports.
- [`docs/phase-1-report.md`](docs/phase-1-report.md),
  [`docs/phase-2-report.md`](docs/phase-2-report.md) and
  [`docs/phase-3-report.md`](docs/phase-3-report.md) and
  [`docs/phase-4-report.md`](docs/phase-4-report.md) and
  [`docs/phase-5-report.md`](docs/phase-5-report.md) - each phase checked
  against its definition of done, with the gaps carried forward.

## Roadmap

| Phase | Deliverable | Status |
|-------|-------------|--------|
| 0 | Repo, build system, CI, test harness | Done |
| 1 | Single-node baseline: pure deterministic core, tests | **Done** |
| 2 | Networking layer: TCP message passing between nodes | **Done** |
| 3 | State replication, primary to backup | **Done** |
| 4 | Leader election | **Done** |
| 5 | Fault-tolerant recovery | **Done** |
| 6 | Benchmarking and hardening | Next |
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
