// Sententia - cluster message types and their binary encoding.
//
// Frame layout, 12-byte header then payload:
//
//   offset  size  field
//   0       4     magic    0x544E4553 ("SENT" little-endian)
//   4       2     version  protocol version
//   6       2     type     MessageType
//   8       4     length   payload byte count
//   12      ...   payload
//
// The length prefix exists because TCP is a byte stream, not a message
// stream. See docs/wire-protocol.md.
//
// Note what CommandForward carries: a Phase 1 Command, unchanged. That
// is not a coincidence. Commands were defined to hold no engine-assigned
// fields precisely so that forwarding one is a straight encode with
// nothing to reconcile between sender and receiver.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "sententia/command.hpp"
#include "sententia/net/wire.hpp"

namespace sententia::net {

constexpr std::uint32_t kMagic = 0x544E4553;  // "SENT"
constexpr std::uint16_t kVersion = 1;
constexpr std::size_t kHeaderSize = 12;

// A hard cap on payload size. The length field arrives from the network
// and is therefore untrusted: without a cap, a peer sending length
// 0xFFFFFFFF would have us try to reserve 4 GiB. This is the difference
// between a malformed message and a denial of service.
constexpr std::uint32_t kMaxPayload = 1u << 20;  // 1 MiB

using NodeId = std::uint32_t;

enum class MessageType : std::uint16_t {
    // In use this phase.
    Hello = 1,
    Heartbeat = 2,
    CommandForward = 3,
    EventAck = 4,

    // Reserved for later phases. Declared now so the type space is
    // stable and an older node can recognise a message it cannot yet
    // handle, rather than treating it as a framing error.
    AppendEntries = 10,   // Phase 3, replication
    AppendResponse = 11,  // Phase 3
    RequestVote = 20,     // Phase 4, leader election
    VoteResponse = 21,    // Phase 4
};

// Sent immediately on connect so each side learns who the other is.
struct Hello {
    NodeId nodeId{};
    std::uint16_t protocolVersion{kVersion};
    std::string advertisedAddress;

    friend bool operator==(const Hello&, const Hello&) = default;
};

// Liveness. The counter is monotonic per sender, so a receiver can spot
// gaps. No timestamp: clocks disagree between machines, and this phase
// establishes no need for one.
struct Heartbeat {
    NodeId nodeId{};
    std::uint64_t counter{};

    friend bool operator==(const Heartbeat&, const Heartbeat&) = default;
};

// A client command being forwarded to the node that will order it. The
// sequence number is the sender's own ordinal, used to detect loss; the
// authoritative ordering is assigned by the leader in Phase 4.
struct CommandForward {
    NodeId originNodeId{};
    std::uint64_t originSeq{};
    Command command{};

    friend bool operator==(const CommandForward&, const CommandForward&) = default;
};

// Acknowledges progress up to a point in the event stream, and carries
// the sender's engine state checksum so two nodes can detect divergence
// cheaply. Phase 3 makes real use of this; here it proves the round trip.
struct EventAck {
    NodeId nodeId{};
    std::uint64_t throughEventSeq{};
    std::uint64_t stateChecksum{};

    friend bool operator==(const EventAck&, const EventAck&) = default;
};

using Message = std::variant<Hello, Heartbeat, CommandForward, EventAck>;

MessageType typeOf(const Message& m) noexcept;

// Encodes a complete frame, header included, appending to `out`.
void encode(const Message& m, Buffer& out);

// Convenience form.
Buffer encode(const Message& m);

// Decodes a payload body. The caller has already validated the header
// and sliced out exactly `size` payload bytes. Returns nullopt on a
// malformed body, an unknown type, or trailing bytes: a message that
// does not consume its payload exactly is treated as corrupt rather
// than silently accepted.
std::optional<Message> decodeBody(MessageType type, const Byte* data, std::size_t size);

const char* toString(MessageType t) noexcept;

// Human-readable one-liner for logging.
std::string describe(const Message& m);

}  // namespace sententia::net
