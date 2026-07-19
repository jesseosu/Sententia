// Backpressure: a peer that stops reading must not be able to grow the
// sender's memory without bound.
//
// This is a regression test for the defect Phase 2 shipped with. There
// was no cap on the outbound queue at all, which was invisible while the
// only traffic was periodic heartbeats and became a memory leak the
// moment Phase 3 started replicating every command. Measured before the
// fix: 65 MB of unsent data and still climbing after 2 million commands
// to a peer that had stopped reading, with bytes actually written flat
// at 4.26 MB from the moment the socket buffer filled.
//
// A primary must not die because a backup got slow.
#include "sententia/net/framing.hpp"

#include <algorithm>
#include "sententia/net/transport.hpp"

#include "harness.hpp"
#include "support.hpp"

using namespace sententia;
using namespace sententia::net;

namespace {

void testWriterRespectsHighWaterMark() {
    FrameWriter writer(1024);
    CHECK(!writer.overHighWaterMark());
    CHECK_EQ(writer.highWaterMark(), std::size_t{1024});

    while (!writer.overHighWaterMark()) {
        writer.enqueue(Message{Heartbeat{1, 1}});
    }
    CHECK(writer.size() >= std::size_t{1024});

    // Draining below the mark clears the condition, so backpressure is
    // a transient state and not a latch.
    writer.consume(writer.size());
    CHECK(!writer.overHighWaterMark());
}

void testSendRefusesRatherThanQueueingForever() {
    Transport a(1, 0);
    Transport b(2, 0);
    std::string error;
    CHECK(a.start(error));
    CHECK(b.start(error));

    // A small mark so the cap is reached quickly and deterministically.
    a.setHighWaterMark(64 * 1024);
    a.addPeer(PeerConfig{2, "127.0.0.1", b.listenPort()});

    for (int i = 0; i < 2000 && a.readyPeerCount() == 0; ++i) {
        a.poll(1);
        b.poll(1);
    }
    CHECK_EQ(a.readyPeerCount(), std::size_t{1});

    // From here b never polls again: it is alive, connected, and not
    // consuming. This is a backup pausing for a GC or a slow disk, not
    // a crash, which is why the connection stays up.
    const auto cmds = support::generateCommands(0x5150ULL, 200000);

    std::size_t accepted = 0;
    std::size_t refused = 0;
    std::size_t peakPending = 0;

    for (std::size_t i = 0; i < cmds.size(); ++i) {
        const SendResult r = a.send(2, Message{CommandForward{1, i, cmds[i]}});
        if (r == SendResult::Ok) {
            ++accepted;
        } else if (r == SendResult::WouldOverflow) {
            ++refused;
        } else {
            break;  // connection died, a different test's concern
        }
        peakPending = std::max(peakPending, a.pendingBytes(2));
    }

    // The queue is bounded. Before the fix this test would have grown
    // to tens of megabytes; the bound is the high-water mark plus at
    // most one message, since the check happens before the enqueue.
    CHECK(peakPending > 0);
    CHECK(peakPending < std::size_t{64 * 1024} + 4096);

    // And the transport told us it was refusing, rather than silently
    // dropping messages or silently growing.
    CHECK(refused > 0);
    CHECK(accepted > 0);
    CHECK_EQ(accepted + refused, cmds.size());
    CHECK(a.stats().sendsRefusedOverflow > 0);
    CHECK(a.isSaturated(2));

    // Once the backup resumes reading, the sender recovers: pressure is
    // relieved and sends are accepted again.
    for (int i = 0; i < 4000 && a.isSaturated(2); ++i) {
        a.poll(1);
        b.poll(1);
    }
    CHECK(!a.isSaturated(2));
    CHECK(a.send(2, Message{Heartbeat{1, 1}}) == SendResult::Ok);
}

void testBroadcastAlsoRespectsTheCap() {
    Transport a(1, 0);
    Transport b(2, 0);
    std::string error;
    CHECK(a.start(error));
    CHECK(b.start(error));
    a.setHighWaterMark(32 * 1024);
    a.addPeer(PeerConfig{2, "127.0.0.1", b.listenPort()});
    for (int i = 0; i < 2000 && a.readyPeerCount() == 0; ++i) {
        a.poll(1);
        b.poll(1);
    }
    CHECK_EQ(a.readyPeerCount(), std::size_t{1});

    std::size_t reached = 0;
    for (int i = 0; i < 200000; ++i) {
        reached += a.broadcast(Message{Heartbeat{1, static_cast<std::uint64_t>(i)}});
    }
    // Broadcast skipped the saturated peer rather than queueing to it.
    CHECK(reached < std::size_t{200000});
    CHECK(a.pendingBytes(2) < std::size_t{32 * 1024} + 4096);
}

void run() {
    testWriterRespectsHighWaterMark();
    testSendRefusesRatherThanQueueingForever();
    testBroadcastAlsoRespectsTheCap();
}

}  // namespace

TEST_MAIN("test_backpressure")
