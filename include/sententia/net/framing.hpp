// Sententia - message framing over a byte stream.
//
// This is the heart of the phase, and it contains no socket code at all.
//
// TCP delivers a stream of bytes, not messages. One write() of 100 bytes
// can arrive as 30 then 70, or two writes can arrive coalesced into one
// read. The bytes arrive in order and complete; the *boundaries* are the
// thing TCP does not preserve. So the boundaries have to be encoded in
// the data, which is what the length prefix is for.
//
// The design decision here mirrors Phase 1's: FrameReader and
// FrameWriter are pure byte-level state machines with no I/O. They do
// not know what a socket is. The socket layer reads bytes and hands them
// to FrameReader; FrameWriter produces bytes and the socket layer writes
// as many as the kernel accepts.
//
// That split is what makes partial reads and writes exhaustively
// testable. tests/test_framing.cpp feeds a multi-message stream one byte
// at a time and asserts the exact messages come out, which is the
// pathological case real networks produce rarely and unrepeatably. No
// sockets, no timing, no flakiness.
#pragma once

#include <cstddef>
#include <optional>

#include "sententia/net/message.hpp"
#include "sententia/net/wire.hpp"

namespace sententia::net {

struct Frame {
    MessageType type{};
    Buffer payload;
};

// Accumulates bytes and yields complete frames.
class FrameReader {
public:
    enum class Status {
        Ok,
        BadMagic,    // stream desynchronised or not our protocol
        BadVersion,  // peer speaks a protocol we do not
        PayloadTooLarge,
    };

    // Feeds raw bytes from the wire. Any number, including zero, and
    // with no relationship to message boundaries.
    void append(const Byte* data, std::size_t size);

    // Yields the next complete frame, or nullopt when the buffered
    // bytes do not yet contain one. Call in a loop: a single read can
    // deliver several messages.
    std::optional<Frame> next();

    // A framing error is unrecoverable for the connection. Once the
    // stream is desynchronised there is no way to find the next real
    // boundary, so the only correct response is to drop the peer.
    bool failed() const noexcept { return status_ != Status::Ok; }
    Status status() const noexcept { return status_; }

    std::size_t buffered() const noexcept { return buf_.size() - readPos_; }
    void reset() noexcept;

private:
    void compact();

    Buffer buf_;
    std::size_t readPos_{0};
    Status status_{Status::Ok};
};

const char* toString(FrameReader::Status s) noexcept;

// Buffers outbound bytes and tracks how many have been written.
//
// It never calls write() itself. The caller writes data()[0..size()) to
// wherever it likes and reports back with consume(). That is what makes
// a short write ordinary rather than exceptional: the kernel took what
// it took, tell the writer, come back when the socket is writable again.
// Default cap on unsent bytes per peer. Phase 2 had no cap at all,
// which was invisible while the only traffic was periodic heartbeats
// and became a memory leak the moment Phase 3 started replicating every
// command: measured 65 MB of unsent data and climbing after 2 million
// commands to a peer that had stopped reading, with bytes actually
// written flat at 4.26 MB. A primary must not die because a backup got
// slow. See docs/failure-modes.md.
constexpr std::size_t kDefaultHighWaterMark = 8u << 20;  // 8 MiB

class FrameWriter {
public:
    explicit FrameWriter(std::size_t highWaterMark = kDefaultHighWaterMark) noexcept
        : highWaterMark_(highWaterMark) {}

    // True when the queue is at or past its cap. The caller decides what
    // that means: a synchronous replicator refuses new work, an
    // asynchronous one drops the peer and lets it catch up from the log.
    // Either way the decision is explicit rather than an allocation.
    bool overHighWaterMark() const noexcept { return size() >= highWaterMark_; }
    std::size_t highWaterMark() const noexcept { return highWaterMark_; }
    void setHighWaterMark(std::size_t bytes) noexcept { highWaterMark_ = bytes; }

    void enqueue(const Message& m);
    void enqueueRaw(const Byte* data, std::size_t size);

    const Byte* data() const noexcept { return buf_.data() + writePos_; }
    std::size_t size() const noexcept { return buf_.size() - writePos_; }
    bool empty() const noexcept { return writePos_ == buf_.size(); }

    // Reports that `n` bytes starting at data() were successfully
    // written. Anything less than size() is a normal short write.
    void consume(std::size_t n);

    std::size_t pending() const noexcept { return size(); }
    void reset() noexcept;

private:
    void compact();

    Buffer buf_;
    std::size_t writePos_{0};
    std::size_t highWaterMark_{kDefaultHighWaterMark};
};

}  // namespace sententia::net
