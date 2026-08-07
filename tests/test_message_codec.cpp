// Message encode/decode: round trips, header layout, and rejection of
// malformed bodies.
#include "sententia/net/message.hpp"

#include <vector>

#include "harness.hpp"
#include "support.hpp"

using namespace sententia;
using namespace sententia::net;

namespace {

// Round-trips a message through a full frame and compares.
template <typename T>
void roundTrip(const T& original) {
    const Buffer frame = encode(Message{original});
    CHECK(frame.size() >= kHeaderSize);

    // Header check.
    WireReader r(frame);
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint16_t type = 0;
    std::uint32_t length = 0;
    CHECK(r.u32(magic) && r.u16(version) && r.u16(type) && r.u32(length));
    CHECK_EQ(magic, kMagic);
    CHECK_EQ(version, kVersion);
    CHECK_EQ(static_cast<MessageType>(type), typeOf(Message{original}));
    // The declared length must match the actual payload exactly.
    CHECK_EQ(std::size_t{length}, frame.size() - kHeaderSize);

    auto decoded = decodeBody(static_cast<MessageType>(type), frame.data() + kHeaderSize, length);
    CHECK(decoded.has_value());
    if (decoded.has_value()) {
        const T* got = std::get_if<T>(&decoded.value());
        CHECK(got != nullptr);
        if (got != nullptr) {
            CHECK(*got == original);
        }
    }
}

void run() {
    // Every message type round-trips. AppendEntries and AppendResponse
    // were added in Phase 3 without round-trip coverage, and when a
    // `term` field was added to both in Phase 4 the encoder was updated
    // and the decoder was not. The result was that every AppendResponse
    // silently failed to decode and was dropped, so replication stopped
    // dead. A round-trip here would have caught it in seconds.
    roundTrip(AppendEntries{3, 1, 10, 5, {}});
    roundTrip(AppendEntries{7,
                            2,
                            0,
                            0,
                            {LogRecord{1, Command{support::limit(1, Side::Buy, 100, 5)}},
                             LogRecord{2, Command{support::cancel(1)}}}});
    roundTrip(AppendResponse{4, 2, true, 99, 123456789});
    roundTrip(AppendResponse{0, 5, false, 0, 0});
    roundTrip(RequestVote{9, 3, 4242});
    roundTrip(RequestVote{0, 1, 0});
    roundTrip(VoteResponse{9, 2, true});
    roundTrip(VoteResponse{9, 2, false});

    roundTrip(Hello{7, kVersion, "127.0.0.1:7101"});
    roundTrip(Hello{1, kVersion, ""});
    roundTrip(Heartbeat{3, 0});
    roundTrip(Heartbeat{4294967295u, 18446744073709551615ull});
    roundTrip(EventAck{2, 99, 1234567890123456789ull});

    // Commands survive the trip unchanged. This is the property that
    // makes forwarding a command to another node meaningful at all.
    roundTrip(CommandForward{1, 5, Command{support::limit(42, Side::Buy, 10050, 7)}});
    roundTrip(CommandForward{
        2, 6, Command{support::limit(43, Side::Sell, 1, 1, TimeInForce::ImmediateOrCancel)}});
    roundTrip(CommandForward{3, 7, Command{support::market(44, Side::Buy, 500)}});
    roundTrip(CommandForward{4, 8, Command{support::cancel(45)}});

    // Negative prices are not valid orders, but the codec must carry
    // them faithfully so the engine gets to be the thing that rejects
    // them. Validation belongs in one place, not two.
    {
        NewOrder odd = support::limit(46, Side::Sell, -5, 3);
        roundTrip(CommandForward{5, 9, Command{odd}});
    }

    // Every generated command round-trips. Reuses the Phase 1 generator
    // so the codec is exercised over the same shapes the engine sees.
    {
        const auto cmds = support::generateCommands(0xFEEDULL, 2000);
        std::size_t mismatches = 0;
        for (std::size_t i = 0; i < cmds.size(); ++i) {
            const CommandForward original{1, i, cmds[i]};
            const Buffer frame = encode(Message{original});
            auto decoded = decodeBody(MessageType::CommandForward, frame.data() + kHeaderSize,
                                      frame.size() - kHeaderSize);
            if (!decoded.has_value() || std::get<CommandForward>(*decoded) != original) {
                ++mismatches;
            }
        }
        CHECK_EQ(mismatches, std::size_t{0});
    }

    // A truncated body is rejected rather than partially accepted.
    {
        const Buffer frame = encode(Message{Heartbeat{9, 12345}});
        for (std::size_t cut = 0; cut < frame.size() - kHeaderSize; ++cut) {
            auto decoded = decodeBody(MessageType::Heartbeat, frame.data() + kHeaderSize, cut);
            CHECK(!decoded.has_value());
        }
    }

    // Trailing bytes are rejected. A body that does not consume itself
    // exactly means sender and receiver disagree about the format, and
    // guessing which of them is right is not an option.
    {
        Buffer frame = encode(Message{Heartbeat{9, 12345}});
        frame.push_back(0xFF);
        auto decoded = decodeBody(MessageType::Heartbeat, frame.data() + kHeaderSize,
                                  frame.size() - kHeaderSize);
        CHECK(!decoded.has_value());
    }

    // An out-of-range enum value in a command is refused. Casting it
    // would be undefined behaviour, and a hostile or buggy peer is a
    // normal thing to encounter.
    {
        Buffer frame =
            encode(Message{CommandForward{1, 1, Command{support::limit(1, Side::Buy, 100, 1)}}});
        // Body layout: originNodeId(4) originSeq(8) tag(1) id(8) instrument(4) side(1)
        const std::size_t sideOffset = kHeaderSize + 4 + 8 + 1 + 8 + 4;
        frame[sideOffset] = 99;
        auto decoded = decodeBody(MessageType::CommandForward, frame.data() + kHeaderSize,
                                  frame.size() - kHeaderSize);
        CHECK(!decoded.has_value());
    }

    // An unknown command discriminant is refused.
    {
        Buffer frame = encode(Message{CommandForward{1, 1, Command{support::cancel(1)}}});
        frame[kHeaderSize + 4 + 8] = 77;
        auto decoded = decodeBody(MessageType::CommandForward, frame.data() + kHeaderSize,
                                  frame.size() - kHeaderSize);
        CHECK(!decoded.has_value());
    }

    // A guard against the gap that caused the bug above: every message
    // type in the variant must be exercised by a round trip. If a new
    // one is added to the variant, this count changes and the assertion
    // below fails until coverage is added for it.
    {
        constexpr std::size_t kMessageTypeCount = std::variant_size_v<Message>;
        CHECK_EQ(kMessageTypeCount, std::size_t{8});

        // Each alternative encodes to a distinct type tag, and each of
        // those tags decodes back to the same alternative.
        const std::vector<Message> oneOfEach = {
            Message{Hello{1, kVersion, "a"}},
            Message{Heartbeat{1, 1}},
            Message{CommandForward{1, 1, Command{support::cancel(1)}}},
            Message{EventAck{1, 1, 1}},
            Message{AppendEntries{1, 1, 0, 0, {}}},
            Message{AppendResponse{1, 1, true, 1, 1}},
            Message{RequestVote{1, 1, 1}},
            Message{VoteResponse{1, 1, true}},
        };
        CHECK_EQ(oneOfEach.size(), kMessageTypeCount);
        for (const Message& m : oneOfEach) {
            const Buffer frame = encode(m);
            auto decoded =
                decodeBody(typeOf(m), frame.data() + kHeaderSize, frame.size() - kHeaderSize);
            CHECK(decoded.has_value());
            if (decoded.has_value()) {
                CHECK_EQ(decoded->index(), m.index());
                CHECK(decoded.value() == m);
            }
        }
    }
}

}  // namespace

TEST_MAIN("test_message_codec")
