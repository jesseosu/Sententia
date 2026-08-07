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
    Hello = 1,
    Heartbeat = 2,
    CommandForward = 3,
    EventAck = 4,

    // Phase 3, replication.
    AppendEntries = 10,
    AppendResponse = 11,

    // Phase 4, leader election.
    RequestVote = 20,
    VoteResponse = 21,
};

// Cap on entries per AppendEntries message. Bounds the payload well
// under kMaxPayload and keeps a catch-up burst from monopolising the
// connection: a backup a million entries behind gets caught up over
// many messages, interleaved with everything else.
constexpr std::size_t kMaxEntriesPerAppend = 512;

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

// One command with the sequence number the primary assigned it. The
// sequence is what the two nodes agree on; the command is unchanged from
// Phase 1, which is why forwarding it needs no reconciliation.
struct LogRecord {
    std::uint64_t seq{};
    Command command{};

    friend bool operator==(const LogRecord&, const LogRecord&) = default;
};

// The primary replicating commands to a backup.
//
// `prevSeq` is the sequence the backup must already have applied for
// these entries to follow on. If it does not match, the backup has a gap
// and says so rather than applying out of order, which would break the
// determinism contract silently and unrecoverably.
//
// An empty `entries` list is a probe: it is how a primary discovers
// where a freshly reconnected backup got to.
struct AppendEntries {
    // The leader's term. A follower that sees a higher term than its own
    // adopts it and steps down; a leader that sees one stops being
    // leader. This single field is what makes a stale leader harmless:
    // its messages are ignored by anyone who has moved on.
    std::uint64_t term{};
    NodeId leaderId{};
    std::uint64_t prevSeq{};
    std::uint64_t commitSeq{};
    std::vector<LogRecord> entries;

    friend bool operator==(const AppendEntries&, const AppendEntries&) = default;
};

// The backup's answer. `ok` false means "I have a gap, resend from
// lastApplied + 1". The checksum lets the primary detect divergence
// cheaply, without ever shipping a book.
struct AppendResponse {
    std::uint64_t term{};
    NodeId nodeId{};
    bool ok{false};
    std::uint64_t lastApplied{};
    std::uint64_t stateChecksum{};

    friend bool operator==(const AppendResponse&, const AppendResponse&) = default;
};

// A candidate asking to be made leader for `term`.
//
// `lastLogSeq` is the election restriction, and it is the subtle part.
// A voter refuses a candidate whose log is behind its own, because a
// leader missing entries that were already committed elsewhere would
// silently drop them. Majority voting alone does not prevent that; this
// check is what does.
struct RequestVote {
    std::uint64_t term{};
    NodeId candidateId{};
    std::uint64_t lastLogSeq{};

    friend bool operator==(const RequestVote&, const RequestVote&) = default;
};

// A vote, or a refusal. The term is carried so a candidate learns it has
// been superseded even when the answer is no.
struct VoteResponse {
    std::uint64_t term{};
    NodeId voterId{};
    bool granted{false};

    friend bool operator==(const VoteResponse&, const VoteResponse&) = default;
};

using Message = std::variant<Hello, Heartbeat, CommandForward, EventAck, AppendEntries,
                             AppendResponse, RequestVote, VoteResponse>;

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
