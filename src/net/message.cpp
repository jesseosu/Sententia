#include "sententia/net/message.hpp"

#include <sstream>

namespace sententia::net {
namespace {

constexpr std::uint32_t kMaxAddressLen = 256;

void encodeCommand(WireWriter& w, const Command& cmd) {
    // A discriminant byte, then the variant body. Explicit rather than
    // relying on variant index ordering, so reordering the variant in a
    // later phase cannot silently change the wire format.
    if (const auto* n = std::get_if<NewOrder>(&cmd)) {
        w.u8(0);
        w.u64(n->id);
        w.u32(n->instrument);
        w.u8(static_cast<std::uint8_t>(n->side));
        w.u8(static_cast<std::uint8_t>(n->type));
        w.u8(static_cast<std::uint8_t>(n->tif));
        w.i64(n->price);
        w.u64(n->quantity);
    } else {
        const auto& c = std::get<CancelOrder>(cmd);
        w.u8(1);
        w.u64(c.id);
    }
}

bool decodeCommand(WireReader& r, Command& out) {
    std::uint8_t tag = 0;
    if (!r.u8(tag)) {
        return false;
    }
    if (tag == 0) {
        NewOrder n;
        std::uint8_t side = 0;
        std::uint8_t type = 0;
        std::uint8_t tif = 0;
        if (!r.u64(n.id) || !r.u32(n.instrument) || !r.u8(side) || !r.u8(type) || !r.u8(tif) ||
            !r.i64(n.price) || !r.u64(n.quantity)) {
            return false;
        }
        // Enum values arrive from the network and must be validated
        // before being cast. An out-of-range cast is undefined
        // behaviour, and "a peer sent us nonsense" is a normal event in
        // a distributed system, not an exceptional one.
        if (side > 1 || type > 1 || tif > 1) {
            return false;
        }
        n.side = static_cast<Side>(side);
        n.type = static_cast<OrderType>(type);
        n.tif = static_cast<TimeInForce>(tif);
        out = n;
        return true;
    }
    if (tag == 1) {
        CancelOrder c;
        if (!r.u64(c.id)) {
            return false;
        }
        out = c;
        return true;
    }
    return false;
}

struct TypeVisitor {
    MessageType operator()(const AppendEntries&) const noexcept {
        return MessageType::AppendEntries;
    }
    MessageType operator()(const AppendResponse&) const noexcept {
        return MessageType::AppendResponse;
    }
    MessageType operator()(const Hello&) const noexcept { return MessageType::Hello; }
    MessageType operator()(const Heartbeat&) const noexcept { return MessageType::Heartbeat; }
    MessageType operator()(const CommandForward&) const noexcept {
        return MessageType::CommandForward;
    }
    MessageType operator()(const EventAck&) const noexcept { return MessageType::EventAck; }
};

struct BodyVisitor {
    WireWriter& w;

    void operator()(const Hello& m) const {
        w.u32(m.nodeId);
        w.u16(m.protocolVersion);
        w.str(m.advertisedAddress);
    }

    void operator()(const Heartbeat& m) const {
        w.u32(m.nodeId);
        w.u64(m.counter);
    }

    void operator()(const CommandForward& m) const {
        w.u32(m.originNodeId);
        w.u64(m.originSeq);
        encodeCommand(w, m.command);
    }

    void operator()(const EventAck& m) const {
        w.u32(m.nodeId);
        w.u64(m.throughEventSeq);
        w.u64(m.stateChecksum);
    }

    void operator()(const AppendEntries& m) const {
        w.u32(m.leaderId);
        w.u64(m.prevSeq);
        w.u64(m.commitSeq);
        w.u32(static_cast<std::uint32_t>(m.entries.size()));
        for (const LogRecord& e : m.entries) {
            w.u64(e.seq);
            encodeCommand(w, e.command);
        }
    }

    void operator()(const AppendResponse& m) const {
        w.u32(m.nodeId);
        w.u8(m.ok ? 1 : 0);
        w.u64(m.lastApplied);
        w.u64(m.stateChecksum);
    }
};

}  // namespace

MessageType typeOf(const Message& m) noexcept {
    return std::visit(TypeVisitor{}, m);
}

void encode(const Message& m, Buffer& out) {
    const std::size_t frameStart = out.size();
    WireWriter w(out);
    w.u32(kMagic);
    w.u16(kVersion);
    w.u16(static_cast<std::uint16_t>(typeOf(m)));
    w.u32(0);  // length placeholder, patched below

    const std::size_t bodyStart = out.size();
    std::visit(BodyVisitor{w}, m);
    const std::size_t bodyLen = out.size() - bodyStart;

    // Patch the length now that the body is encoded, little-endian to
    // match how it will be read back.
    const auto len = static_cast<std::uint32_t>(bodyLen);
    for (int i = 0; i < 4; ++i) {
        out[frameStart + 8 + static_cast<std::size_t>(i)] =
            static_cast<Byte>((len >> (i * 8)) & 0xFF);
    }
}

Buffer encode(const Message& m) {
    Buffer out;
    encode(m, out);
    return out;
}

std::optional<Message> decodeBody(MessageType type, const Byte* data, std::size_t size) {
    WireReader r(data, size);

    switch (type) {
        case MessageType::Hello: {
            Hello m;
            if (!r.u32(m.nodeId) || !r.u16(m.protocolVersion) ||
                !r.str(m.advertisedAddress, kMaxAddressLen)) {
                return std::nullopt;
            }
            if (!r.ok() || !r.exhausted()) {
                return std::nullopt;
            }
            return Message{m};
        }
        case MessageType::Heartbeat: {
            Heartbeat m;
            if (!r.u32(m.nodeId) || !r.u64(m.counter) || !r.ok() || !r.exhausted()) {
                return std::nullopt;
            }
            return Message{m};
        }
        case MessageType::CommandForward: {
            CommandForward m;
            if (!r.u32(m.originNodeId) || !r.u64(m.originSeq) || !decodeCommand(r, m.command)) {
                return std::nullopt;
            }
            if (!r.ok() || !r.exhausted()) {
                return std::nullopt;
            }
            return Message{m};
        }
        case MessageType::EventAck: {
            EventAck m;
            if (!r.u32(m.nodeId) || !r.u64(m.throughEventSeq) || !r.u64(m.stateChecksum) ||
                !r.ok() || !r.exhausted()) {
                return std::nullopt;
            }
            return Message{m};
        }
        case MessageType::AppendEntries: {
            AppendEntries m;
            std::uint32_t count = 0;
            if (!r.u32(m.leaderId) || !r.u64(m.prevSeq) || !r.u64(m.commitSeq) || !r.u32(count)) {
                return std::nullopt;
            }
            // Untrusted count. Cap it before it becomes a reserve, for
            // the same reason the frame length is capped.
            if (count > kMaxEntriesPerAppend) {
                return std::nullopt;
            }
            m.entries.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i) {
                LogRecord e;
                if (!r.u64(e.seq) || !decodeCommand(r, e.command)) {
                    return std::nullopt;
                }
                m.entries.push_back(std::move(e));
            }
            if (!r.ok() || !r.exhausted()) {
                return std::nullopt;
            }
            return Message{std::move(m)};
        }
        case MessageType::AppendResponse: {
            AppendResponse m;
            std::uint8_t ok = 0;
            if (!r.u32(m.nodeId) || !r.u8(ok) || !r.u64(m.lastApplied) || !r.u64(m.stateChecksum) ||
                !r.ok() || !r.exhausted()) {
                return std::nullopt;
            }
            if (ok > 1) {
                return std::nullopt;
            }
            m.ok = ok == 1;
            return Message{m};
        }

        // Reserved for later phases: recognised as a type, not yet
        // decodable. Returning nullopt here is correct; the transport
        // reports it as an unhandled type rather than a framing error.
        case MessageType::RequestVote:
        case MessageType::VoteResponse:
            return std::nullopt;
    }
    return std::nullopt;
}

const char* toString(MessageType t) noexcept {
    switch (t) {
        case MessageType::Hello:
            return "HELLO";
        case MessageType::Heartbeat:
            return "HEARTBEAT";
        case MessageType::CommandForward:
            return "COMMAND_FORWARD";
        case MessageType::EventAck:
            return "EVENT_ACK";
        case MessageType::AppendEntries:
            return "APPEND_ENTRIES";
        case MessageType::AppendResponse:
            return "APPEND_RESPONSE";
        case MessageType::RequestVote:
            return "REQUEST_VOTE";
        case MessageType::VoteResponse:
            return "VOTE_RESPONSE";
    }
    return "UNKNOWN";
}

std::string describe(const Message& m) {
    std::ostringstream os;
    os << toString(typeOf(m)) << ' ';
    std::visit(
        [&os](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, Hello>) {
                os << "node=" << v.nodeId << " version=" << v.protocolVersion
                   << " addr=" << v.advertisedAddress;
            } else if constexpr (std::is_same_v<T, Heartbeat>) {
                os << "node=" << v.nodeId << " counter=" << v.counter;
            } else if constexpr (std::is_same_v<T, CommandForward>) {
                os << "origin=" << v.originNodeId << " seq=" << v.originSeq << ' ';
                if (const auto* n = std::get_if<NewOrder>(&v.command)) {
                    os << "NEW id=" << n->id << " side=" << sententia::toString(n->side)
                       << " px=" << n->price << " qty=" << n->quantity;
                } else {
                    os << "CANCEL id=" << std::get<CancelOrder>(v.command).id;
                }
            } else if constexpr (std::is_same_v<T, AppendEntries>) {
                os << "leader=" << v.leaderId << " prev=" << v.prevSeq << " commit=" << v.commitSeq
                   << " entries=" << v.entries.size();
            } else if constexpr (std::is_same_v<T, AppendResponse>) {
                os << "node=" << v.nodeId << " ok=" << (v.ok ? "yes" : "no")
                   << " applied=" << v.lastApplied << " checksum=" << v.stateChecksum;
            } else {
                os << "node=" << v.nodeId << " through=" << v.throughEventSeq
                   << " checksum=" << v.stateChecksum;
            }
        },
        m);
    return os.str();
}

}  // namespace sententia::net
