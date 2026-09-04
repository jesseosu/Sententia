// Transport integration: real sockets over loopback.
//
// Two Transports in one process, connected to each other, driven by
// pumping their poll loops. Everything below this level was tested
// without sockets; this test exists to prove the socket plumbing is
// wired to that machinery correctly, and to cover what only a real
// connection can produce: a peer vanishing mid-stream.
//
// It uses port 0 so the kernel assigns free ports, which is what keeps
// the test from colliding with anything else on the machine or with a
// previous run still in TIME_WAIT.
#include "sententia/net/transport.hpp"

#include <string>
#include <vector>

#include "harness.hpp"
#include "support.hpp"

using namespace sententia;
using namespace sententia::net;

namespace {

// Drives both transports until `done` holds or the budget runs out.
// Bounded by iterations, not by wall-clock time: a test that waits on a
// duration is a test that fails on a loaded CI machine.
bool pump(Transport& a, Transport& b, const std::function<bool()>& done, int maxCycles = 2000) {
    for (int i = 0; i < maxCycles; ++i) {
        a.poll(1);
        b.poll(1);
        if (done()) {
            return true;
        }
    }
    return false;
}

void testHandshakeAndHeartbeat() {
    Transport a(1, 0);
    Transport b(2, 0);
    std::string error;
    CHECK(a.start(error));
    CHECK(b.start(error));
    CHECK(a.listenPort() != 0);
    CHECK(b.listenPort() != 0);

    // Only node 1 dials. The inbound side learns who node 1 is from the
    // Hello, which is the whole reason Hello exists.
    a.addPeer(PeerConfig{2, "127.0.0.1", b.listenPort()});

    std::vector<Message> receivedByA;
    std::vector<Message> receivedByB;
    std::vector<NodeId> upOnA;
    std::vector<NodeId> upOnB;

    a.onMessage([&](NodeId, const Message& m) { receivedByA.push_back(m); });
    b.onMessage([&](NodeId, const Message& m) { receivedByB.push_back(m); });
    a.onPeerUp([&](NodeId id, const std::string&) { upOnA.push_back(id); });
    b.onPeerUp([&](NodeId id, const std::string&) { upOnB.push_back(id); });

    CHECK(pump(a, b, [&] { return a.readyPeerCount() == 1 && b.readyPeerCount() == 1; }));
    CHECK_EQ(a.readyPeerCount(), std::size_t{1});
    CHECK_EQ(b.readyPeerCount(), std::size_t{1});

    // Each side learned the other's real node id.
    CHECK_EQ(upOnA.size(), std::size_t{1});
    CHECK_EQ(upOnB.size(), std::size_t{1});
    if (!upOnA.empty()) {
        CHECK_EQ(upOnA[0], NodeId{2});
    }
    if (!upOnB.empty()) {
        CHECK_EQ(upOnB[0], NodeId{1});
    }
    CHECK(a.isReady(2));
    CHECK(b.isReady(1));
    CHECK(!a.isReady(99));

    // Hello is consumed by the transport itself, so the application
    // handler never sees one.
    CHECK_EQ(receivedByA.size(), std::size_t{0});
    CHECK_EQ(receivedByB.size(), std::size_t{0});

    // A heartbeat crosses and arrives intact.
    CHECK(a.send(2, Message{Heartbeat{1, 77}}) == SendResult::Ok);
    CHECK(pump(a, b, [&] { return !receivedByB.empty(); }));
    CHECK_EQ(receivedByB.size(), std::size_t{1});
    if (!receivedByB.empty()) {
        const auto* hb = std::get_if<Heartbeat>(&receivedByB[0]);
        CHECK(hb != nullptr);
        if (hb != nullptr) {
            CHECK_EQ(hb->nodeId, NodeId{1});
            CHECK_EQ(hb->counter, std::uint64_t{77});
        }
    }

    // Sending to a peer that is not connected fails cleanly.
    CHECK(a.send(99, Message{Heartbeat{1, 1}}) == SendResult::NotConnected);
}

void testManyMessagesPreserveOrder() {
    Transport a(1, 0);
    Transport b(2, 0);
    std::string error;
    CHECK(a.start(error));
    CHECK(b.start(error));
    a.addPeer(PeerConfig{2, "127.0.0.1", b.listenPort()});

    std::vector<Message> got;
    b.onMessage([&](NodeId, const Message& m) { got.push_back(m); });
    CHECK(pump(a, b, [&] { return a.readyPeerCount() == 1 && b.readyPeerCount() == 1; }));

    // Enough traffic to guarantee the kernel buffers fill and short
    // writes actually happen, exercising the partial-write path against
    // a real socket rather than only in the unit test.
    const auto cmds = support::generateCommands(0xABCDULL, 4000);
    for (std::size_t i = 0; i < cmds.size(); ++i) {
        a.send(2, Message{CommandForward{1, i, cmds[i]}});
        // Interleave polls so the writer drains as it goes.
        if (i % 64 == 0) {
            a.poll(0);
            b.poll(0);
        }
    }

    CHECK(pump(a, b, [&] { return got.size() == cmds.size(); }, 20000));
    CHECK_EQ(got.size(), cmds.size());

    // Order and content preserved exactly. TCP guarantees byte order;
    // this checks the framing preserved *message* order on top of it.
    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < got.size() && i < cmds.size(); ++i) {
        const auto* cf = std::get_if<CommandForward>(&got[i]);
        if (cf == nullptr || cf->originSeq != i || cf->command != cmds[i]) {
            ++mismatches;
        }
    }
    CHECK_EQ(mismatches, std::size_t{0});

    // Note on short writes: on loopback the kernel absorbs this much
    // traffic without ever refusing a write, so asserting that a short
    // write happened here would be asserting a property of the local
    // network stack rather than of this code. Measured: zero short
    // writes even at 50,000 messages. The partial-write path is covered
    // deterministically instead, by FrameWriter across every step size
    // in test_framing, and against a real socket with a deliberately
    // small send buffer in testPartialWriteAgainstRealSocket below.
    CHECK_EQ(a.stats().framingErrors, std::uint64_t{0});
    CHECK_EQ(b.stats().framingErrors, std::uint64_t{0});
    CHECK_EQ(b.stats().decodeErrors, std::uint64_t{0});
}

// Forces the kernel to refuse a write, which is the condition the
// transport's short-write branch exists to handle. A tiny send buffer
// makes it happen on demand rather than depending on traffic volume.
// Regression: in a mesh both nodes dial each other, so each pair ends
// up with two TCP connections unless something resolves the duplicate.
// The first version of this transport had exactly that bug, and it
// showed up as every heartbeat being delivered twice. In Phase 3 that
// would mean applying every replicated command twice.
void testMutualDialProducesOneConnection() {
    Transport a(1, 0);
    Transport b(2, 0);
    std::string error;
    CHECK(a.start(error));
    CHECK(b.start(error));

    // Both sides dial. This is the normal mesh configuration, not a
    // misconfiguration.
    a.addPeer(PeerConfig{2, "127.0.0.1", b.listenPort()});
    b.addPeer(PeerConfig{1, "127.0.0.1", a.listenPort()});

    std::vector<Message> gotByA;
    std::vector<Message> gotByB;
    a.onMessage([&](NodeId, const Message& m) { gotByA.push_back(m); });
    b.onMessage([&](NodeId, const Message& m) { gotByB.push_back(m); });

    CHECK(pump(a, b, [&] { return a.readyPeerCount() >= 1 && b.readyPeerCount() >= 1; }));

    // Let any duplicate settle before asserting.
    for (int i = 0; i < 200; ++i) {
        a.poll(1);
        b.poll(1);
    }

    // Exactly one connection survives on each side, not two.
    CHECK_EQ(a.readyPeerCount(), std::size_t{1});
    CHECK_EQ(b.readyPeerCount(), std::size_t{1});
    CHECK(a.isReady(2));
    CHECK(b.isReady(1));

    // And each message crosses exactly once.
    CHECK(a.send(2, Message{Heartbeat{1, 1}}) == SendResult::Ok);
    CHECK(b.send(1, Message{Heartbeat{2, 1}}) == SendResult::Ok);
    CHECK(pump(a, b, [&] { return !gotByA.empty() && !gotByB.empty(); }));
    for (int i = 0; i < 200; ++i) {
        a.poll(1);
        b.poll(1);
    }
    CHECK_EQ(gotByA.size(), std::size_t{1});
    CHECK_EQ(gotByB.size(), std::size_t{1});

    // The connection stays stable rather than flapping between the two
    // directions: no further disconnects once settled.
    const std::uint64_t disconnectsA = a.stats().disconnects;
    for (int i = 0; i < 300; ++i) {
        a.poll(1);
        b.poll(1);
    }
    CHECK_EQ(a.stats().disconnects, disconnectsA);
    CHECK_EQ(a.readyPeerCount(), std::size_t{1});
}

void testPartialWriteAgainstRealSocket() {
    std::string error;
    Socket listener = Socket::listen(0, 4, error);
    CHECK(listener.valid());
    const std::uint16_t port = listener.localPort();

    Socket client = Socket::connect("127.0.0.1", port, error);
    CHECK(client.valid());
    CHECK(client.setSendBufferSize(4096, error));

    IoStatus status = IoStatus::Ok;
    Socket server;
    for (int i = 0; i < 1000 && !server.valid(); ++i) {
        server = listener.accept(status);
    }
    CHECK(server.valid());
    // An accepted socket does NOT inherit O_NONBLOCK from its listener.
    // Forgetting this is how a "non-blocking" server ends up blocking
    // forever on a read, which is exactly what happened when this test
    // was first written. Transport::acceptPending does the same thing.
    CHECK(server.setNonBlocking(error));
    CHECK(server.setRecvBufferSize(4096, error));

    // Write far more than either buffer can hold, without reading. The
    // kernel must eventually take less than offered, then refuse
    // outright, which is exactly EAGAIN on a non-blocking socket.
    const std::vector<Byte> blob(1 << 20, 0xAB);
    std::size_t offset = 0;
    bool sawShortWrite = false;
    bool sawWouldBlock = false;

    for (int i = 0; i < 10000 && offset < blob.size(); ++i) {
        const std::size_t remaining = blob.size() - offset;
        const IoResult r = client.write(blob.data() + offset, remaining);
        if (r.status == IoStatus::WouldBlock) {
            sawWouldBlock = true;
            break;
        }
        CHECK(r.status == IoStatus::Ok);
        if (r.bytes < remaining) {
            sawShortWrite = true;
        }
        offset += r.bytes;
    }

    // Both halves of the contract: the kernel took less than offered,
    // and then refused entirely. Neither is an error.
    CHECK(sawShortWrite);
    CHECK(sawWouldBlock);
    CHECK(offset > 0);
    CHECK(offset < blob.size());

    // Draining the receiver makes the sender writable again, and the
    // bytes that did land arrived intact and in order.
    std::vector<Byte> sink(65536);
    std::size_t received = 0;
    bool corrupted = false;
    // Retry on WouldBlock rather than giving up at the first one.
    //
    // The previous version broke out immediately, which assumed the
    // bytes were already sitting in the receive buffer. On Linux
    // loopback they are. On macOS they may not be yet, so the first read
    // returned WouldBlock, the loop exited having read nothing, and
    // `received > 0` failed. That broke CI on macOS for five commits.
    //
    // Same mistake as the short-write assertion and the saturation
    // check: an assertion bounded by how fast the local network stack
    // happens to be. Bounded by iterations here, never by a sleep.
    int emptyReads = 0;
    for (int i = 0; i < 200000 && emptyReads < 20000; ++i) {
        const IoResult r = server.read(sink.data(), sink.size());
        if (r.status == IoStatus::WouldBlock) {
            ++emptyReads;
            if (received > 0 && emptyReads > 1000) {
                break;  // drained what was sent
            }
            continue;
        }
        if (r.status != IoStatus::Ok) {
            break;
        }
        for (std::size_t j = 0; j < r.bytes; ++j) {
            if (sink[j] != 0xAB) {
                corrupted = true;
            }
        }
        received += r.bytes;
    }
    CHECK(!corrupted);
    CHECK(received > 0);

    // With the receiver drained, the sender can make progress again.
    const IoResult resumed = client.write(blob.data() + offset, blob.size() - offset);
    CHECK(resumed.status == IoStatus::Ok || resumed.status == IoStatus::WouldBlock);
}

void testPeerDisconnectIsSurvivable() {
    std::string error;
    std::vector<std::string> downReasons;

    Transport a(1, 0);
    CHECK(a.start(error));
    a.onPeerDown([&](NodeId, const std::string& why) { downReasons.push_back(why); });

    {
        Transport b(2, 0);
        CHECK(b.start(error));
        a.addPeer(PeerConfig{2, "127.0.0.1", b.listenPort()});
        CHECK(pump(a, b, [&] { return a.readyPeerCount() == 1 && b.readyPeerCount() == 1; }));
        CHECK_EQ(a.readyPeerCount(), std::size_t{1});
        // b goes out of scope here: sockets close, node 2 is gone.
    }

    // The surviving node must notice and keep running. In later phases
    // nodes are killed deliberately, so this is the behaviour the whole
    // project depends on rather than an edge case.
    bool noticed = false;
    for (int i = 0; i < 2000 && !noticed; ++i) {
        CHECK(a.poll(1));
        noticed = a.readyPeerCount() == 0;
    }
    CHECK(noticed);
    CHECK(!downReasons.empty());
    CHECK(a.stats().disconnects > 0);

    // Still alive and still accepting work: sending to the dead peer
    // fails cleanly instead of crashing or blocking.
    CHECK(a.send(2, Message{Heartbeat{1, 1}}) == SendResult::NotConnected);
    CHECK(a.poll(1));
}

void testConnectToNothingIsSurvivable() {
    // Dialling a port with nothing behind it must not crash or wedge.
    // A node starting before its peers is the normal case, not an error.
    Transport a(1, 0);
    std::string error;
    CHECK(a.start(error));
    a.setReconnectDelayCycles(1);
    a.addPeer(PeerConfig{2, "127.0.0.1", 1});  // port 1: nothing listens here

    for (int i = 0; i < 50; ++i) {
        CHECK(a.poll(1));
    }
    CHECK_EQ(a.readyPeerCount(), std::size_t{0});
}

void testGarbageInputDropsPeerNotProcess() {
    // A peer that speaks nonsense must cost us that connection and
    // nothing more.
    Transport a(1, 0);
    std::string error;
    CHECK(a.start(error));

    Socket rogue = Socket::connect("127.0.0.1", a.listenPort(), error);
    CHECK(rogue.valid());

    for (int i = 0; i < 100; ++i) {
        a.poll(1);
    }

    const char junk[] = "this is definitely not a sententia frame, not even close";
    rogue.write(junk, sizeof(junk));

    bool dropped = false;
    for (int i = 0; i < 500 && !dropped; ++i) {
        a.poll(1);
        dropped = a.stats().framingErrors > 0;
    }
    CHECK(dropped);
    // The transport is still alive and still listening.
    CHECK(a.poll(1));
    CHECK_EQ(a.readyPeerCount(), std::size_t{0});
}

void run() {
    testHandshakeAndHeartbeat();
    testManyMessagesPreserveOrder();
    testMutualDialProducesOneConnection();
    testPartialWriteAgainstRealSocket();
    testPeerDisconnectIsSurvivable();
    testConnectToNothingIsSurvivable();
    testGarbageInputDropsPeerNotProcess();
}

}  // namespace

TEST_MAIN("test_transport")
