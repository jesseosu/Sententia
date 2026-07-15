# Wire Protocol

The message format nodes use to talk to each other, and the reasoning
behind each part of it.

## The framing problem

TCP is a **byte stream**, not a message stream. This is the single most
important fact about writing network code on top of it, and the source
of most of the bugs.

If a node calls `write()` once with a 100-byte message, the peer might
see:

- one `read()` returning all 100 bytes, or
- `read()` returning 30, then 70, or
- `read()` returning 100 bytes that are the last 40 of one message and
  the first 60 of the next, or
- any other split.

TCP guarantees the bytes arrive, in order, without duplication or
corruption. It guarantees nothing at all about **where one message ends
and the next begins**. Those boundaries are not preserved because they
were never transmitted: `write()` is not a message primitive.

So the boundaries have to be encoded in the data itself. Two options are
common: a delimiter (a byte that cannot appear in the payload, which
means escaping the payload), or a length prefix (say up front how many
bytes follow). Length prefixes win for binary data, because binary
payloads contain every byte value and there is no delimiter to reserve.

## Frame layout

Every message is a 12-byte header followed by a payload.

```
 offset  size  field     notes
 ------  ----  --------  -----------------------------------------
 0       4     magic     0x544E4553, "SENT" little-endian
 4       2     version   protocol version, currently 1
 6       2     type      MessageType
 8       4     length    payload byte count
 12      ...   payload   `length` bytes
```

**magic** catches a desynchronised stream and a client that connected to
the wrong port. It is a sanity check, not security.

**version** lets a node reject a peer running an incompatible build
explicitly, rather than misparsing its messages. Reported separately
from a magic mismatch so an operator can tell "wrong protocol" from
"wrong build".

**length** is what makes the stream parseable. It is also the most
dangerous field in the header, because it arrives from the network and
is used as an allocation size. See below.

## Byte order is explicit

Every integer is written and read one byte at a time, little-endian.
Not `memcpy` of a struct, not `htonl`, not a cast over a buffer.

This is the same decision as Phase 1's integer tick prices, for the same
reason. A replica must decode a message to the identical value the
sender encoded, on a different machine and possibly a different
architecture. Anything that leaks the host's byte order, struct padding,
or alignment rules into the wire format is a divergence waiting to
happen, and it will not show up until the cluster is heterogeneous.

Shifting bytes explicitly costs a few instructions and is completely
unambiguous. `sizeof` never appears in the encoder.

Signed values cross the wire as two's complement through a `uint64_t`
cast, which is well defined in both directions. Reinterpreting the
object representation would not be.

## The untrusted length field

A malicious or simply buggy peer can send a header claiming a payload of
`0xFFFFFFFF` bytes. If the receiver reserves that, the node dies on a
single 12-byte message.

So `length` is checked against `kMaxPayload` (1 MiB) **before** it is
used for anything. This is the difference between a malformed message
and a denial of service. The same reasoning applies to the length prefix
on strings inside a payload, which is why `WireReader::str` takes an
explicit cap.

More generally: every field decoded from the wire is validated before
use. Enum values are range-checked before being cast, because casting an
out-of-range value into an enum is undefined behaviour, and "a peer sent
nonsense" is a routine event in a distributed system rather than an
exceptional one.

## Message types

| Type | Value | Purpose |
|------|-------|---------|
| `Hello` | 1 | Identifies the sender. First message on every connection. |
| `Heartbeat` | 2 | Liveness, with a monotonic per-sender counter. |
| `CommandForward` | 3 | A Phase 1 `Command`, forwarded to another node. |
| `EventAck` | 4 | Progress plus a state checksum, for divergence detection. |
| `AppendEntries` | 10 | Reserved, Phase 3. |
| `AppendResponse` | 11 | Reserved, Phase 3. |
| `RequestVote` | 20 | Reserved, Phase 4. |
| `VoteResponse` | 21 | Reserved, Phase 4. |

The reserved values are declared now so the type space is stable. A node
that receives a type it cannot decode logs it and **keeps the
connection**: the frame boundary was correct, so the stream is still in
sync. That is different from a framing error, which is unrecoverable.

Note that `CommandForward` carries a Phase 1 `Command` unchanged. That is
not a coincidence. Commands were defined to hold no engine-assigned
fields precisely so that forwarding one is a straight encode with
nothing to reconcile between sender and receiver.

## Heartbeats carry no timestamp

A heartbeat has a monotonic counter, not a clock reading. Two machines'
clocks disagree, and a timestamp that crosses the network invites code
that compares it to the local clock and draws conclusions from the
difference. Phase 1 removed wall-clock time from the engine for exactly
this reason; reintroducing it in the transport would be a step backwards.

Nothing in this phase needs to know what time it is. When something does,
it will need a logical clock, not a physical one.

## Connections

Each node listens on its configured port and dials every other node in
the cluster config. Membership is static, from a file: service discovery
is a genuinely hard problem in its own right and solving it here would
obscure the phases that actually need solving.

### Handshake

On connect, both sides immediately send `Hello`. The transport consumes
it rather than passing it to the application, because it is how the
transport learns who is on the other end of an *inbound* socket, which
has no config entry to identify it. Any message received before `Hello`
drops the connection.

### Duplicate connections

Because every node dials every other node, each pair ends up with **two**
TCP connections, one in each direction. Left alone, every message is
delivered twice. This is not hypothetical: the first version of this
transport had the bug, and it surfaced immediately in the two-node demo
as duplicate heartbeats.

The tiebreak has to be decidable by both sides independently, with no
negotiation, and reach the same answer. The rule:

> **Keep the connection dialed by the lower-numbered node.**

Node ids are unique and both sides know both ids after `Hello`, so both
compute the same winner and drop the same loser. A node also declines to
dial a peer it already has a live connection to, which stops the losing
side from redialling forever.

### Disconnects

A peer closing is an ordinary event, not an error. `read()` returning 0
means an orderly shutdown; `EPIPE` and `ECONNRESET` mean the peer went
away less politely. All three drop the peer and, if we dialed it,
schedule a redial. Writes use `MSG_NOSIGNAL` so a write to a dead peer
returns `EPIPE` instead of raising `SIGPIPE` and killing the process.
This matters a great deal in Phase 5, where nodes are killed on purpose.

## Multiplexing: poll(), not epoll()

A cluster is three to five nodes, so the descriptor set is tiny.

`poll()` is O(n) per call because the kernel rescans the whole set. At
n=5 that is irrelevant. `epoll()` is O(1) in the number of *ready*
descriptors and wins decisively at hundreds or thousands of connections,
because it keeps the interest set in the kernel across calls instead of
copying it every time.

`poll()` is also portable across every POSIX system including macOS,
where `epoll` is Linux-only and the equivalent is `kqueue`.

So: `poll()` here, and revisit if a node ever fans out to many clients.
That is a real difference at scale and no difference at all at this size.

## Platform

The networking layer is POSIX only. The engine core stays portable and
CI still builds and tests it on Windows, macOS and Linux, including the
cross-platform determinism check.

A Winsock shim would be real work for no signal: this project deploys to
Linux, and Phase 5 pins threads with `SCHED_FIFO` there anyway.
