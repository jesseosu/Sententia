# Phase 2 Completion Report: Networking Layer

Checked against the must-haves, should-haves and definition of done in
`03_PHASE2_NETWORKING.md`.

## Must-haves

| Requirement | Status | Where |
|-------------|--------|-------|
| Length-framed message protocol, documented | Done | `include/sententia/net/message.hpp`, `docs/wire-protocol.md` |
| Node can listen, accept, and connect to peers from config | Done | `src/net/socket.cpp`, `src/net/transport.cpp`, `src/net/cluster_config.cpp` |
| Reliable send/receive handling partial reads and writes | Done | `src/net/framing.cpp`, tested exhaustively in `tests/test_framing.cpp` |
| Graceful handling of peer disconnect (no crash) | Done | `Transport::dropPeer`, `MSG_NOSIGNAL`, `testPeerDisconnectIsSurvivable` |
| Two-node heartbeat demo works; integration test passes | Done | `apps/node/`, `scripts/two_node_demo.sh`, ctest `two_node_demo` |

## Should-haves

| Requirement | Status | Where |
|-------------|--------|-------|
| Configurable cluster membership from a file | Done | `src/net/cluster_config.cpp`, `scripts/cluster.conf` |
| Message-level logging | Done | `Transport::onLog`, `describe()`, plus `TransportStats` counters |
| A "cluster up" script launching N nodes locally | Done | `scripts/cluster_up.sh` |

## Definition of done

> You can start two (or more) node processes, they connect, and they
> reliably exchange structured messages over TCP, including surviving a
> peer disconnecting.

Met. `ctest` runs 16 tests, all green, on GCC and Clang.

```
$ ./scripts/cluster_up.sh
starting node 1
starting node 2
starting node 3
[node 1] PEER UP 2 at 0.0.0.0:7102
[node 1] PEER UP 3 at 0.0.0.0:7103
[node 2] RECV from 1: HEARTBEAT node=1 counter=1
...
```

## The design decision that mattered

**Framing has no I/O in it.** `FrameReader` and `FrameWriter` are pure
byte-level state machines that do not know what a socket is. The socket
layer reads bytes and hands them over; the writer produces bytes and the
socket layer writes as many as the kernel accepts.

This is the Phase 1 pure-core discipline applied one layer up, and it
pays off the same way. `tests/test_framing.cpp` feeds a multi-message
stream **one byte at a time**, and at every chunk size from 1 to the full
stream length, asserting the identical message sequence comes out. That
is the pathological case real networks produce rarely and
unrepeatably. Here it is a deterministic loop with no sockets, no timing,
and no flakiness.

Four of the five network test binaries open no socket at all.

## Two bugs this phase found

**Duplicate connections.** In a mesh, every node dials every other node,
so each pair ends up with two TCP connections and every message is
delivered twice. The two-node demo surfaced it immediately as duplicate
heartbeat counters. In Phase 3 it would have meant applying every
replicated command twice.

Fixed with a tiebreak that both sides compute independently and agree on
without negotiating: keep the connection dialed by the lower-numbered
node. A node also declines to dial a peer it already has a live
connection to, so the losing side does not redial forever. Regression
test: `testMutualDialProducesOneConnection`.

**Accepted sockets do not inherit `O_NONBLOCK`.** A socket returned by
`accept()` is blocking even when its listener is not. The production code
handles it; the first version of the partial-write test did not, and hung
forever on a read. Worth recording because the failure mode is a hang
rather than an error, which makes it hard to diagnose from a CI timeout.

## Testing notes

**Short writes do not happen on loopback at small message sizes.** The
first version of the transport test asserted `shortWrites > 0` after
sending 4,000 messages. It failed. Measured: zero short writes even at
**50,000 messages**, because the kernel's loopback buffers absorb it all.

Asserting it there would have been asserting a property of the local
network stack, not of this code. So the partial-write path is covered two
ways instead: `FrameWriter` across every step size from 1 to 64 in
`test_framing`, and against a real socket with a deliberately small
`SO_SNDBUF` in `testPartialWriteAgainstRealSocket`, which forces both a
short write and an `EAGAIN` on demand.

**Tests are bounded by iterations, not wall-clock time.** A test that
waits on a duration is a test that fails on a loaded CI machine. Ports
are all 0, so the kernel assigns free ones and runs cannot collide with
each other or with a previous run still in `TIME_WAIT`.

## Test inventory

| Test | Sockets | Validates |
|------|---------|-----------|
| `test_wire` | no | Exact byte layout, round trips, bounds checks, untrusted lengths |
| `test_message_codec` | no | Frame headers, round trips, truncation, trailing bytes, bad enums |
| `test_framing` | no | Every chunk size 1..N, coalesced reads, split headers and bodies, bad magic, bad version, oversized payloads, short writes at every step size |
| `test_cluster_config` | no | Parsing, malformed lines, duplicate ids, port ranges |
| `test_transport` | yes | Handshake, 4,000-message ordering, duplicate-connection tiebreak, real partial writes, peer death, garbage input, dialing nothing |
| `two_node_demo` | yes | Two real processes, end to end |

## Known gaps, carried forward deliberately

- **No reconnection backoff curve.** Retry is a fixed cycle count, not
  exponential backoff with jitter. Fine for a static cluster on a LAN;
  revisit if it ever runs across a WAN.
- **IPv4 only.** `inet_pton` with `AF_INET`. IPv6 is a small change and
  no distributed-systems interest.
- **No TLS, no authentication.** A cluster is assumed to be on a trusted
  network. Worth stating rather than leaving implied.
- **No message-level flow control.** A slow peer causes unbounded growth
  in its `FrameWriter` queue. Phase 3 needs a real answer here, because
  replication is where a backed-up peer actually happens.
- **The engine is not wired in yet.** `apps/node` exchanges heartbeats
  and nothing else. Connecting the matching engine to the transport is
  Phase 3's job, and doing it now would mean guessing at a replication
  design before the transport under it had been proven.

## What this sets up

- **Phase 3 (replication)** ships commands, not state. `CommandForward`
  already carries a Phase 1 `Command` unchanged, and `EventAck` already
  carries a state checksum, which is the primary/backup agreement check.
- **Phase 4 (leader election)** needs message types that already have
  reserved slots in the enum, and needs the total ordering that the
  Phase 1 sensitivity test showed the engine is sensitive to.
- **Phase 5 (recovery)** needs a node to survive its peers dying, which
  `testPeerDisconnectIsSurvivable` and `MSG_NOSIGNAL` handling establish.
