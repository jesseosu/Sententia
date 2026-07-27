// State-machine replication, end to end over real sockets.
//
// The claim under test is the one the whole project rests on:
//
//   A backup that applies the same commands in the same order as the
//   primary reaches byte-identical state, and converges to it again even
//   after disconnecting mid-stream and missing an arbitrary number of
//   commands.
//
// "Identical state" is checked with the Phase 1 state checksum, which
// test_order_book already proved is a function of state rather than of
// history. That is what makes it a legitimate agreement check between
// two nodes that took different paths to the same place.
#include "sententia/replication/replicator.hpp"

#include <functional>
#include <memory>
#include <vector>

#include "harness.hpp"
#include "support.hpp"

using namespace sententia;
using namespace sententia::net;
using namespace sententia::replication;

namespace {

constexpr InstrumentId kInstrument = support::kInstrument;

// One node: engine, log, transport, replicator, wired together the way
// apps/node does it.
struct Node {
    MatchingEngine engine{kInstrument};
    CommandLog log;
    Transport transport;
    std::unique_ptr<Replicator> replicator;

    Node(NodeId id, Role role, ReplicationMode mode) : transport(id, 0) {
        std::string error;
        transport.start(error);
        replicator = std::make_unique<Replicator>(id, role, mode, engine, log, transport);
        transport.onMessage(
            [this](NodeId from, const Message& m) { replicator->onMessage(from, m); });
        transport.onPeerUp([this](NodeId peer, const std::string&) { replicator->onPeerUp(peer); });
        transport.onPeerDown(
            [this](NodeId peer, const std::string&) { replicator->onPeerDown(peer); });
    }

    void poll(int timeoutMs = 0) {
        transport.poll(timeoutMs);
        replicator->tick();
    }
};

bool pump(Node& a, Node& b, const std::function<bool()>& done, int maxCycles = 20000) {
    for (int i = 0; i < maxCycles; ++i) {
        a.poll(1);
        b.poll(1);
        if (done()) {
            return true;
        }
    }
    return false;
}

void connect(Node& primary, Node& backup) {
    primary.transport.addPeer(
        PeerConfig{backup.transport.selfId(), "127.0.0.1", backup.transport.listenPort()});
    pump(primary, backup, [&] {
        return primary.transport.readyPeerCount() == 1 && backup.transport.readyPeerCount() == 1;
    });
}

// Feeds a command stream through the primary, retrying on Busy, and
// waits for the backup to catch up.
void replicateAll(Node& primary, Node& backup, const std::vector<Command>& cmds) {
    std::size_t next = 0;
    for (int cycle = 0; cycle < 400000 && next < cmds.size(); ++cycle) {
        const SubmitResult r = primary.replicator->submit(cmds[next]);
        if (r.ok()) {
            ++next;
        }
        primary.poll(0);
        backup.poll(0);
    }
    CHECK_EQ(next, cmds.size());
    pump(primary, backup, [&] {
        return backup.replicator->lastApplied() == primary.log.lastSeq() &&
               primary.replicator->lastApplied() == primary.log.lastSeq();
    });
}

void testAsyncConvergence() {
    Node primary(1, Role::Primary, ReplicationMode::Asynchronous);
    Node backup(2, Role::Backup, ReplicationMode::Asynchronous);
    connect(primary, backup);
    CHECK(primary.replicator->hasBackup());

    const auto cmds = support::generateCommands(0xAA51ULL, 3000);
    replicateAll(primary, backup, cmds);

    CHECK_EQ(backup.replicator->lastApplied(), primary.replicator->lastApplied());
    CHECK_EQ(primary.replicator->lastApplied(), Sequence{cmds.size()});
    // The whole point: identical state, never having shipped any state.
    CHECK_EQ(backup.engine.stateChecksum(), primary.engine.stateChecksum());
    CHECK_EQ(backup.engine.book().checksum(), primary.engine.book().checksum());
    CHECK_EQ(primary.replicator->stats().checksumMismatches, std::uint64_t{0});

    // And a genuinely non-trivial book, so this is not converging on
    // two empty engines.
    CHECK(primary.engine.book().orderCount() > 50);
}

void testSyncConvergenceAndCommitSemantics() {
    Node primary(1, Role::Primary, ReplicationMode::Synchronous);
    Node backup(2, Role::Backup, ReplicationMode::Synchronous);
    connect(primary, backup);

    // Strict lockstep: one unacknowledged command at a time.
    primary.replicator->setSyncWindow(1);

    const auto cmds = support::generateCommands(0xBEE1ULL, 800);
    replicateAll(primary, backup, cmds);

    CHECK_EQ(backup.engine.stateChecksum(), primary.engine.stateChecksum());
    CHECK_EQ(primary.replicator->lastApplied(), Sequence{cmds.size()});
    CHECK_EQ(backup.replicator->lastApplied(), Sequence{cmds.size()});

    // Under synchronous replication the primary never runs ahead of what
    // the backup has acknowledged. This is the guarantee the mode exists
    // to provide, so it is worth asserting rather than assuming.
    CHECK(primary.replicator->lastApplied() <= primary.replicator->commitSeq());
    CHECK(primary.replicator->stats().submitRejectedBusy > 0);
}

void testSyncRefusesWithoutABackup() {
    // With no backup, applying anyway would silently downgrade to
    // asynchronous and quietly drop the guarantee the caller asked for.
    Node lonely(1, Role::Primary, ReplicationMode::Synchronous);
    const SubmitResult r = lonely.replicator->submit(support::limit(1, Side::Buy, 100, 10));
    CHECK(r.status == SubmitStatus::NoBackup);
    CHECK_EQ(lonely.replicator->lastApplied(), Sequence{0});
    CHECK_EQ(lonely.engine.book().orderCount(), std::size_t{0});
}

void testBackupRefusesClientCommands() {
    Node backup(2, Role::Backup, ReplicationMode::Asynchronous);
    const SubmitResult r = backup.replicator->submit(support::limit(1, Side::Buy, 100, 10));
    CHECK(r.status == SubmitStatus::NotPrimary);
}

void testCatchUpAfterDisconnect() {
    Node primary(1, Role::Primary, ReplicationMode::Asynchronous);
    auto backup = std::make_unique<Node>(2, Role::Backup, ReplicationMode::Asynchronous);
    const std::uint16_t backupPort = backup->transport.listenPort();
    connect(primary, *backup);

    const auto cmds = support::generateCommands(0xCA74ULL, 4000);

    // Phase 1: replicate the first slice normally.
    std::vector<Command> firstSlice(cmds.begin(), cmds.begin() + 800);
    replicateAll(primary, *backup, firstSlice);
    const Sequence caughtUpAt = backup->replicator->lastApplied();
    CHECK_EQ(caughtUpAt, Sequence{800});
    CHECK_EQ(backup->engine.stateChecksum(), primary.engine.stateChecksum());

    // Phase 2: the backup dies. The primary must keep accepting work.
    backup.reset();
    for (int i = 0; i < 500 && primary.transport.readyPeerCount() > 0; ++i) {
        primary.poll(1);
    }
    CHECK_EQ(primary.transport.readyPeerCount(), std::size_t{0});

    std::size_t acceptedWhileDown = 0;
    for (std::size_t i = 800; i < 2400; ++i) {
        if (primary.replicator->submit(cmds[i]).ok()) {
            ++acceptedWhileDown;
        }
        primary.poll(0);
    }
    // Async: the primary is not blocked by a dead backup.
    CHECK_EQ(acceptedWhileDown, std::size_t{1600});
    CHECK_EQ(primary.replicator->lastApplied(), Sequence{2400});

    // Phase 3: a fresh backup takes the same address and must catch up
    // from scratch, replaying all 2400 commands it never saw.
    Node revived(2, Role::Backup, ReplicationMode::Asynchronous);
    CHECK_EQ(revived.replicator->lastApplied(), Sequence{0});

    // Rebind the primary's peer entry to the revived backup's port.
    Node& backup2 = revived;
    primary.transport.addPeer(PeerConfig{2, "127.0.0.1", backup2.transport.listenPort()});
    (void)backupPort;

    const bool converged = pump(
        primary, backup2,
        [&] { return backup2.replicator->lastApplied() == primary.log.lastSeq(); }, 60000);
    CHECK(converged);
    CHECK_EQ(backup2.replicator->lastApplied(), Sequence{2400});
    // Converged to identical state despite having missed everything.
    CHECK_EQ(backup2.engine.stateChecksum(), primary.engine.stateChecksum());
    CHECK(primary.replicator->stats().catchUpBatches > 0);

    // Phase 4: keep going afterwards, so catch-up left the stream in a
    // state that normal replication can continue from.
    std::vector<Command> tail(cmds.begin() + 2400, cmds.end());
    replicateAll(primary, backup2, tail);
    CHECK_EQ(backup2.replicator->lastApplied(), Sequence{cmds.size()});
    CHECK_EQ(backup2.engine.stateChecksum(), primary.engine.stateChecksum());
    CHECK_EQ(primary.replicator->stats().checksumMismatches, std::uint64_t{0});
}

void testGapDetection() {
    // A backup handed entries that do not follow on must refuse them and
    // report its real position, rather than applying out of order.
    Node backup(2, Role::Backup, ReplicationMode::Asynchronous);

    net::AppendEntries msg;
    msg.leaderId = 1;
    msg.prevSeq = 50;  // backup is at 0, so this is a gap of 50
    msg.entries.push_back(net::LogRecord{51, support::limit(1, Side::Buy, 100, 10)});
    backup.replicator->onMessage(1, Message{msg});

    CHECK_EQ(backup.replicator->lastApplied(), Sequence{0});
    CHECK_EQ(backup.replicator->stats().gapsDetected, std::uint64_t{1});
    CHECK_EQ(backup.engine.book().orderCount(), std::size_t{0});

    // The correct follow-on is accepted.
    net::AppendEntries good;
    good.leaderId = 1;
    good.prevSeq = 0;
    good.entries.push_back(net::LogRecord{1, support::limit(1, Side::Buy, 100, 10)});
    backup.replicator->onMessage(1, Message{good});
    CHECK_EQ(backup.replicator->lastApplied(), Sequence{1});
    CHECK_EQ(backup.engine.book().orderCount(), std::size_t{1});
}

// Replication must not amplify: N commands should cost about N entries
// and about N messages, not orders of magnitude more.
//
// This is a regression test for a use-after-move that left the "how far
// have I sent" cursor permanently at zero, so every submit re-sent the
// entire outstanding range. It was invisible to every correctness test
// here, because re-sending changes no state and the backup converged
// perfectly. It showed up only as 883,003 messages and 91 MB to
// replicate 1,000 commands.
//
// Correctness tests do not catch waste. Something has to assert cost.
void testNoMessageAmplification() {
    Node primary(1, Role::Primary, ReplicationMode::Asynchronous);
    Node backup(2, Role::Backup, ReplicationMode::Asynchronous);
    connect(primary, backup);

    const auto cmds = support::generateCommands(0xA11FULL, 2000);
    replicateAll(primary, backup, cmds);
    CHECK_EQ(backup.engine.stateChecksum(), primary.engine.stateChecksum());

    const auto& rs = primary.replicator->stats();
    const auto& ts = primary.transport.stats();

    // Every command is sent exactly once in the steady state. A little
    // slack for the probe and for a batch that overlaps a retry.
    CHECK(rs.entriesSent >= std::uint64_t{2000});
    CHECK(rs.entriesSent < std::uint64_t{2200});

    // And the message count stays proportional. Before the fix this was
    // roughly 900 times the command count.
    CHECK(ts.messagesSent < std::uint64_t{2200});

    // Bytes on the wire stay proportional too: a command encodes to a
    // few dozen bytes, so a few hundred KB is generous for 2,000.
    CHECK(ts.bytesSent < std::uint64_t{400000});
}

void testLogCompaction() {
    Node primary(1, Role::Primary, ReplicationMode::Asynchronous);
    Node backup(2, Role::Backup, ReplicationMode::Asynchronous);
    connect(primary, backup);

    const auto cmds = support::generateCommands(0xC0C0ULL, 2000);
    replicateAll(primary, backup, cmds);
    CHECK_EQ(primary.log.size(), std::size_t{2000});

    // Only what the backup has acknowledged may be dropped.
    primary.replicator->compactLog();
    CHECK(primary.log.size() < std::size_t{2000});
    CHECK_EQ(primary.log.lastSeq(), Sequence{2000});
    CHECK(primary.replicator->stats().logTruncations > 0);

    // Replication still works after compaction.
    const auto more = support::generateCommands(0xD0D0ULL, 200);
    replicateAll(primary, backup, more);
    CHECK_EQ(backup.engine.stateChecksum(), primary.engine.stateChecksum());
}

void run() {
    testAsyncConvergence();
    testSyncConvergenceAndCommitSemantics();
    testSyncRefusesWithoutABackup();
    testBackupRefusesClientCommands();
    testGapDetection();
    testNoMessageAmplification();
    testCatchUpAfterDisconnect();
    testLogCompaction();
}

}  // namespace

TEST_MAIN("test_replication")
