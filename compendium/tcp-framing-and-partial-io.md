# TCP Framing and Partial I/O

> TCP promises your bytes arrive, in order, uncorrupted. It promises nothing about where one message ends and the next begins, because that boundary was never sent.

**Learned during:** Sententia Phase 2, networking layer
**Tags:** `networking` `tcp` `sockets` `protocol-design` `systems`

---

## The problem it solves

Two processes need to exchange structured messages. TCP is right there, it is reliable, and the obvious thing works on the first try:

```c
write(fd, &msg, sizeof(msg));      // sender
read(fd, &msg, sizeof(msg));       // receiver
```

This appears to work. It will keep appearing to work through local testing, through the demo, and often through the first weeks in production. Then it corrupts data under load and nobody can reproduce it.

The reason is that TCP is a **byte stream**, not a message stream. `write()` is not a message primitive. It hands bytes to the kernel, and the kernel sends them when it feels like it, in whatever sized chunks it likes. A single 100-byte `write()` can reach the peer as one 100-byte `read()`, or 30 then 70, or as 40 bytes that are the tail of the previous message followed by the first 60 of this one.

Everything TCP guarantees is about the byte sequence: it arrives, in order, without duplication or corruption. Message boundaries are not in that list because they were never transmitted. The sender knew where the message ended. That knowledge did not go into the socket.

So it has to be put into the data explicitly. That is framing.

## The core idea

Say up front how many bytes are coming.

```
+--------+------------------+
| length |     payload      |
+--------+------------------+
```

The receiver reads the length, then reads exactly that many bytes, then starts over. It buffers whatever arrives and only produces a message when it has a complete one.

The alternative is a delimiter: pick a byte that means "message over". This works for text protocols (HTTP uses `\r\n\r\n`, and line-based protocols use `\n`) and fails for binary ones, because a binary payload contains every possible byte value including your delimiter. You then need escaping, and escaping means the payload length changes during encoding, and now you have two problems.

For binary, length prefixes win. The full header in Sententia is twelve bytes:

```
 offset  size  field
 0       4     magic     catches a desynchronised stream or wrong port
 4       2     version   reject an incompatible peer explicitly
 6       2     type      which message this is
 8       4     length    payload byte count
 12      ...   payload
```

## How it works

The receiving side is a small state machine over a growing buffer:

```
append whatever arrived to the buffer

loop:
    if buffered < HEADER_SIZE:      return, wait for more
    parse header (peek, do not consume)
    if magic wrong:                 fatal, drop the connection
    if length > MAX_PAYLOAD:        fatal, drop the connection
    if buffered < HEADER + length:  return, wait for more
    emit the message
    advance past HEADER + length
    continue
```

Two things about this loop are easy to get wrong.

**One read is not one message.** After a single `read()` the buffer may hold three complete messages and part of a fourth. So it is a loop, not an `if`. Handling only the first message per read means the rest sit in the buffer until more traffic arrives to push them out, which produces a protocol that mysteriously stalls when it goes quiet.

**A partial message is not an error.** Returning early with an incomplete buffer is the normal case, hit constantly. The code path that does nothing is the hot one.

The sending side has the mirror problem. `write()` returns how many bytes it actually took, which can be fewer than offered when the kernel's send buffer is full:

```
n = write(fd, buf + offset, remaining)
if n == -1 and errno == EAGAIN:  socket is full, try again when writable
offset += n                       // n may be less than remaining
```

Treating a short write as an error, or assuming `write()` wrote everything, truncates messages under exactly the conditions where it is hardest to debug: under load.

## The key insight

The insight is not the length prefix. That part is in every tutorial.

The insight is that **framing and I/O are separable, and separating them is what makes framing testable**.

The tempting design has one class that owns the socket, reads from it, parses frames, and dispatches. It is cohesive and it feels right. It is also nearly untestable, because every test needs a real socket, and the interesting cases (a header split across two reads, a body split one byte before the end, a write the kernel only half accepts) depend on kernel buffer behaviour you cannot make happen on demand.

So in Sententia the framing layer contains no I/O at all. `FrameReader` accepts a pointer and a length and yields complete frames. `FrameWriter` produces bytes and tracks a position; the caller reports back how many were consumed. Neither knows what a socket is.

That inversion turns the hardest cases into a loop:

```cpp
for (std::size_t chunk = 1; chunk <= stream.size(); ++chunk) {
    // feed the whole stream in chunk-sized pieces, assert the same
    // messages come out every time
}
```

Chunk size 1 is the worst thing TCP can do to you, delivered one byte at a time, and it runs in microseconds with no sockets and no flakiness. The same trick works on the write side: drain the writer in one-byte steps and assert the byte sequence is identical.

This is the same discipline as keeping a matching engine free of clocks and randomness so its behaviour is a pure function of its input. Push the non-deterministic thing (there, time; here, the kernel) to the edges, and the core becomes something you can test exhaustively instead of hopefully.

## The tradeoffs

**A length prefix is an attack surface.** The field arrives from the network and is used as an allocation size. A peer sending `0xFFFFFFFF` kills a node with a twelve-byte message unless the length is checked against a cap *before* it is used. This is not a hypothetical hardening step; it is the difference between a malformed message and a denial of service, and it is one line.

**Framing costs a copy, or careful lifetime management.** Yielding a frame either copies the payload out of the buffer or hands back a pointer into it that is invalidated by the next append. Sententia copies, which is the wrong call for a latency-sensitive path and the right one for a phase whose job is correctness.

**Buffers grow unless you compact them.** A long-lived connection consuming from the front of a vector leaks memory in slow motion. Sliding the remainder down past a threshold is cheap and easy to forget.

**No flow control comes for free.** If a peer is slow, its outbound queue grows without bound. Length-prefixed framing says nothing about backpressure; that is a separate problem and it needs a separate answer.

## How I used it in the project

`FrameReader` and `FrameWriter` in Sententia's transport, sitting between a thin POSIX socket wrapper and a `poll()` loop that multiplexes a handful of peers.

`poll()` rather than `epoll()`, deliberately. A cluster is three to five nodes. `poll()` is O(n) because the kernel rescans the interest set on every call; `epoll()` keeps that set in the kernel and is O(ready). At n=5 the difference is nothing, and `poll()` is portable to macOS where `epoll` does not exist. At n=5000 the answer flips completely. Worth being able to say which regime you are in and why.

Every integer crosses the wire as explicit little-endian bytes, shifted one at a time. No `memcpy` of a struct, no `htonl`, no casting a pointer over a buffer. A replica has to decode to the identical value the sender encoded, on a different machine and possibly a different architecture, and anything that leaks host byte order, struct padding, or alignment into the format is a divergence waiting to happen. `sizeof` never appears in the encoder.

Four of the five network test binaries open no socket at all.

## What surprised me

**Short writes do not happen on loopback.** I wrote a test asserting that after sending 4,000 messages the transport would have recorded at least one short write. It failed. I raised it to 50,000 and it still recorded zero: the kernel's loopback buffers absorbed everything.

The assertion was wrong, not the code. It was asserting a property of the local network stack rather than of my transport. The fix was to stop hoping volume would produce the condition and force it instead: set `SO_SNDBUF` to 4 KiB and write a megabyte. Short write and `EAGAIN` both appear within two iterations, deterministically.

The general lesson is one I keep relearning. If a test only passes when the environment cooperates, it is not testing what you think, and it will fail on someone else's machine for reasons that have nothing to do with the code.

**Accepted sockets do not inherit `O_NONBLOCK`.** The listener is non-blocking. The socket that comes back from `accept()` on that listener is not. I knew this and had handled it correctly in the transport, then wrote a test that forgot it, and the test hung forever on a read with no error and no output. The failure mode is a hang rather than a message, which makes it genuinely nasty to diagnose from a CI timeout.

**The mesh gave me two connections per pair, and I did not see it coming.** Every node dials every other node, so A dials B and B dials A, and both succeed. Two TCP connections, every message delivered twice. It surfaced the moment I ran the two-node demo and saw heartbeat counters repeating.

What makes it interesting is the shape of the fix. Both sides have to independently choose the same connection to keep, with no negotiation, because a negotiation would itself need a connection. The rule is one line: keep the connection dialed by the lower-numbered node. Node ids are unique, both sides know both ids after the handshake, so both compute the same winner. The first thing that actually felt like distributed systems rather than networking: a decision made in two places at once that has to come out the same way.

Had it survived to Phase 3, it would have meant applying every replicated command twice.

## References

- Stevens, Fenner, Rudoff, *UNIX Network Programming, Vol. 1*, ch. 3. The `readn`/`writen` helpers exist precisely because partial reads and writes are the norm.
- Kerrisk, *The Linux Programming Interface*, ch. 61 on socket buffer behaviour and ch. 63 on multiplexing.
- [`../systems/lockfree-spsc-queues.md`](../systems/lockfree-spsc-queues.md) - the in-process equivalent of this problem, where the ring buffer's power-of-two mask plays the role the length prefix plays here.
- [`./determinism-and-replication.md`](./determinism-and-replication.md) - the Phase 1 entry. Same discipline of pushing the non-deterministic thing to the edges, applied one layer down.
