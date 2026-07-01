// Framing over a byte stream.
//
// This is where the phase's central claim gets tested: that message
// boundaries survive any way TCP chooses to chop up the stream. Because
// FrameReader and FrameWriter contain no I/O, the pathological splits
// that real networks produce rarely and unrepeatably can be produced
// here deterministically and exhaustively.
#include "sententia/net/framing.hpp"

#include <vector>

#include "harness.hpp"
#include "support.hpp"

using namespace sententia;
using namespace sententia::net;

namespace {

std::vector<Message> sampleMessages() {
    return {
        Message{Hello{1, kVersion, "127.0.0.1:7101"}},
        Message{Heartbeat{1, 1}},
        Message{CommandForward{1, 1, Command{support::limit(10, Side::Buy, 10000, 5)}}},
        Message{Heartbeat{1, 2}},
        Message{EventAck{1, 42, 987654321}},
        Message{CommandForward{1, 2, Command{support::cancel(10)}}},
        Message{Hello{2, kVersion, ""}},
    };
}

Buffer encodeAll(const std::vector<Message>& msgs) {
    Buffer out;
    for (const Message& m : msgs) {
        encode(m, out);
    }
    return out;
}

// Feeds a byte stream to a reader in fixed-size chunks and collects
// every message that comes out.
std::vector<Message> readInChunks(const Buffer& stream, std::size_t chunkSize) {
    FrameReader reader;
    std::vector<Message> got;
    std::size_t offset = 0;
    while (offset < stream.size()) {
        const std::size_t n = std::min(chunkSize, stream.size() - offset);
        reader.append(stream.data() + offset, n);
        offset += n;
        while (auto frame = reader.next()) {
            auto decoded = decodeBody(frame->type, frame->payload.data(), frame->payload.size());
            if (decoded.has_value()) {
                got.push_back(*decoded);
            }
        }
    }
    return got;
}

void run() {
    const auto expected = sampleMessages();
    const Buffer stream = encodeAll(expected);

    // The core claim: every chunk size from one byte up to the whole
    // stream yields the identical message sequence. Chunk size 1 is the
    // worst case TCP can produce and the one that breaks naive parsers.
    for (std::size_t chunk = 1; chunk <= stream.size(); ++chunk) {
        const auto got = readInChunks(stream, chunk);
        CHECK_EQ(got.size(), expected.size());
        bool same = got.size() == expected.size();
        for (std::size_t i = 0; same && i < got.size(); ++i) {
            same = got[i] == expected[i];
        }
        CHECK(same);
    }

    // Several messages arriving coalesced in a single read, which is the
    // other half of the problem: one read is not one message either.
    {
        FrameReader reader;
        reader.append(stream.data(), stream.size());
        std::size_t count = 0;
        while (auto frame = reader.next()) {
            ++count;
        }
        CHECK_EQ(count, expected.size());
        CHECK_EQ(reader.buffered(), std::size_t{0});
    }

    // A header split across two reads yields nothing until complete,
    // and crucially does not error.
    {
        FrameReader reader;
        reader.append(stream.data(), 5);
        CHECK(!reader.next().has_value());
        CHECK(!reader.failed());
        reader.append(stream.data() + 5, kHeaderSize - 5);
        // Header complete, body not.
        CHECK(!reader.next().has_value());
        CHECK(!reader.failed());
    }

    // A body split mid-way yields nothing until the last byte arrives.
    {
        const Buffer one = encode(Message{Heartbeat{5, 55}});
        FrameReader reader;
        reader.append(one.data(), one.size() - 1);
        CHECK(!reader.next().has_value());
        CHECK(!reader.failed());
        reader.append(one.data() + one.size() - 1, 1);
        auto frame = reader.next();
        CHECK(frame.has_value());
        if (frame.has_value()) {
            CHECK(frame->type == MessageType::Heartbeat);
        }
    }

    // Zero-length appends are harmless.
    {
        FrameReader reader;
        reader.append(nullptr, 0);
        CHECK(!reader.failed());
        CHECK_EQ(reader.buffered(), std::size_t{0});
    }

    // Bad magic is fatal for the connection. Once the stream is
    // desynchronised there is no way to find the next real boundary.
    {
        Buffer bad = encode(Message{Heartbeat{1, 1}});
        bad[0] ^= 0xFF;
        FrameReader reader;
        reader.append(bad.data(), bad.size());
        CHECK(!reader.next().has_value());
        CHECK(reader.failed());
        CHECK(reader.status() == FrameReader::Status::BadMagic);
    }

    // A version mismatch is reported distinctly from a magic mismatch,
    // so an operator can tell "wrong protocol" from "wrong build".
    {
        Buffer bad = encode(Message{Heartbeat{1, 1}});
        bad[4] = 0xEE;
        bad[5] = 0xEE;
        FrameReader reader;
        reader.append(bad.data(), bad.size());
        CHECK(!reader.next().has_value());
        CHECK(reader.failed());
        CHECK(reader.status() == FrameReader::Status::BadVersion);
    }

    // An absurd length is rejected before it becomes an allocation.
    // Without this check a single malformed header is a 4 GiB reserve.
    {
        Buffer bad = encode(Message{Heartbeat{1, 1}});
        bad[8] = 0xFF;
        bad[9] = 0xFF;
        bad[10] = 0xFF;
        bad[11] = 0xFF;
        FrameReader reader;
        reader.append(bad.data(), bad.size());
        CHECK(!reader.next().has_value());
        CHECK(reader.failed());
        CHECK(reader.status() == FrameReader::Status::PayloadTooLarge);
    }

    // A failed reader stays failed and accepts no further input.
    {
        Buffer bad = encode(Message{Heartbeat{1, 1}});
        bad[0] ^= 0xFF;
        FrameReader reader;
        reader.append(bad.data(), bad.size());
        CHECK(!reader.next().has_value());
        const Buffer good = encode(Message{Heartbeat{2, 2}});
        reader.append(good.data(), good.size());
        CHECK(!reader.next().has_value());
        CHECK(reader.failed());
    }

    // A good message followed by a corrupt one: the good one is
    // delivered first, then the reader fails. Partial progress is not
    // lost just because what follows is garbage.
    {
        Buffer combined = encode(Message{Heartbeat{1, 1}});
        Buffer bad = encode(Message{Heartbeat{2, 2}});
        bad[0] ^= 0xFF;
        combined.insert(combined.end(), bad.begin(), bad.end());
        FrameReader reader;
        reader.append(combined.data(), combined.size());
        CHECK(reader.next().has_value());
        CHECK(!reader.next().has_value());
        CHECK(reader.failed());
    }

    // FrameWriter: short writes of every size drain the same bytes in
    // the same order. This is the write-side mirror of the chunk loop.
    {
        for (std::size_t step = 1; step <= 64; ++step) {
            FrameWriter writer;
            for (const Message& m : expected) {
                writer.enqueue(m);
            }
            Buffer drained;
            while (!writer.empty()) {
                const std::size_t n = std::min(step, writer.size());
                drained.insert(drained.end(), writer.data(), writer.data() + n);
                writer.consume(n);
            }
            CHECK(drained == stream);
            CHECK_EQ(writer.pending(), std::size_t{0});
        }
    }

    // consume(0) is legal and makes no progress, which is what a socket
    // reporting EAGAIN before writing anything looks like.
    {
        FrameWriter writer;
        writer.enqueue(Message{Heartbeat{1, 1}});
        const std::size_t before = writer.size();
        writer.consume(0);
        CHECK_EQ(writer.size(), before);
    }

    // Over-consuming is clamped rather than corrupting the position.
    {
        FrameWriter writer;
        writer.enqueue(Message{Heartbeat{1, 1}});
        writer.consume(writer.size() + 1000);
        CHECK(writer.empty());
    }

    // Enqueuing while a partial write is outstanding keeps ordering.
    {
        FrameWriter writer;
        writer.enqueue(Message{Heartbeat{1, 1}});
        Buffer drained;
        drained.insert(drained.end(), writer.data(), writer.data() + 3);
        writer.consume(3);
        writer.enqueue(Message{Heartbeat{1, 2}});
        while (!writer.empty()) {
            drained.push_back(*writer.data());
            writer.consume(1);
        }
        Buffer wanted = encode(Message{Heartbeat{1, 1}});
        const Buffer second = encode(Message{Heartbeat{1, 2}});
        wanted.insert(wanted.end(), second.begin(), second.end());
        CHECK(drained == wanted);
    }

    // A large payload near the cap still frames correctly, so the cap
    // is a bound and not an off-by-one that rejects valid traffic.
    {
        Hello big{1, kVersion, std::string(200, 'x')};
        const Buffer frame = encode(Message{big});
        for (std::size_t chunk = 1; chunk <= 8; ++chunk) {
            const auto got = readInChunks(frame, chunk);
            CHECK_EQ(got.size(), std::size_t{1});
            if (got.size() == 1) {
                CHECK(std::get<Hello>(got[0]) == big);
            }
        }
    }
}

}  // namespace

TEST_MAIN("test_framing")
