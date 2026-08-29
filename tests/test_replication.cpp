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

// THE ORACLE.
//
// Every convergence assertion in this file used to compare the leader
// against the follower and nothing else. That checks AGREEMENT, which is
// not the same as CORRECTNESS: a bug that breaks both nodes identically
// passes every one of them.
//
// This is not hypothetical. The mutation harness broke the apply loop so
// that one command in five hundred was skipped, and every convergence
// test still passed, because leader and follower skipped exactly the
// same ones and agreed perfectly on a wrong book.
//
// So there is now a third party: a single-process engine applying the
// same commands with no replication involved at all. The cluster must
// match IT, not just itself.
std::uint64_t referenceChecksum(const std::vector<Command>& cmds) {
    MatchingEngine reference(kInstrument);
    EventList sink;
    for (const Command& c : cmds) {
        sink.clear();
        reference.apply(c, sink);
    }
    return reference.stateChecksum();
}

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
    // And identical to what a single process would have computed. This
    // is the assertion that catches a bug breaking both nodes the same
    // way, which agreement alone cannot see.
    CHECK_EQ(primary.engine.stateChecksum(), referenceChecksum(cmds));
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
    CHECK_EQ(primary.engine.stateChecksum(), referenceChecksum(cmds));
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
    // The leader keeps ACCEPTING work into its log while the backup is
    // gone. It does not block.
    CHECK_EQ(acceptedWhileDown, std::size_t{1600});
    CHECK_EQ(primary.log.lastSeq(), Sequence{2400});

    // But it does NOT apply any of it, and this is the Phase 5 change
    // that matters most.
    //
    // Phase 3 applied on submit, so a leader that crashed after applying
    // but before replicating would have shown a client a trade that no
    // surviving node had. Now an entry is applied only once a MAJORITY
    // holds it. This cluster is configured as two nodes, and a majority
    // of two is two, so with the backup dead nothing can commit.
    //
    // That is not a limitation of the code, it is arithmetic, and it is
    // exactly why real clusters use odd sizes: two nodes tolerate ZERO
    // failures, the same as one. Three tolerate one.
    CHECK_EQ(primary.replicator->lastApplied(), Sequence{800});
    CHECK_EQ(primary.replicator->commitIndex(), Sequence{800});

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
    // With a majority available again, everything the leader accepted
    // while alone commits and both nodes apply it.
    CHECK_EQ(backup2.replicator->lastApplied(), Sequence{2400});
    CHECK_EQ(primary.replicator->lastApplied(), Sequence{2400});
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
    msg.entries.push_back(net::LogRecord{51, 1, support::limit(1, Side::Buy, 100, 10)});
    backup.replicator->onMessage(1, Message{msg});

    CHECK_EQ(backup.replicator->lastApplied(), Sequence{0});
    CHECK_EQ(backup.replicator->stats().gapsDetected, std::uint64_t{1});
    CHECK_EQ(backup.engine.book().orderCount(), std::size_t{0});

    // The correct follow-on is accepted.
    net::AppendEntries good;
    good.leaderId = 1;
    good.prevSeq = 0;
    // A follower now applies only up to the leader's commit point, never
    // to the end of its own log. Entries past the commit point may still
    // be truncated after a leader change, and applying them early would
    // let a client observe a trade that later un-happens. So the leader
    // must say what is committed; without this the entry is appended to
    // the log and correctly not applied.
    good.commitSeq = 1;
    good.entries.push_back(net::LogRecord{1, 0, support::limit(1, Side::Buy, 100, 10)});
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
    //
    // The bound rose from ~1 message per command in Phase 3 to ~2 in
    // Phase 5, and the extra one is real work rather than waste: after
    // an entry is acknowledged the leader commits it, and the follower
    // has to be TOLD the commit point moved or it never applies what it
    // already holds. Measured: exactly 2.00 messages and 1.00 entries
    // per command, so nothing is being re-sent. A real deployment folds
    // this into the election heartbeat instead of sending it separately.
    CHECK(ts.messagesSent < std::uint64_t{2000 * 2 + 200});

    // Bytes on the wire stay proportional too: a command encodes to a
    // few dozen bytes, so a few hundred KB is generous for 2,000.
    CHECK(ts.bytesSent < std::uint64_t{400000});
}

// LOG MATCHING. A follower must check the TERM at prevSeq, not just the
// sequence number. Nothing exercised this until the mutation harness
// pointed out that removing the term comparison changed no test result.
//
// It went unguarded because no test ever produced divergent logs, and
// divergence needs a leader change writing a different entry at a
// sequence another node already holds. So the conflict is injected
// directly rather than staged through an election.
void testLogMatchingRejectsAWrongTerm() {
    Node follower(2, Role::Backup, ReplicationMode::Asynchronous);
    follower.replicator->setTerm(2);

    // Give it three entries from term 2.
    net::AppendEntries first;
    first.term = 2;
    first.leaderId = 1;
    first.prevSeq = 0;
    first.prevTerm = 0;
    first.commitSeq = 3;
    for (std::uint64_t i = 1; i <= 3; ++i) {
        first.entries.push_back(
            net::LogRecord{i, 2, support::limit(i, Side::Buy, static_cast<Price>(100 + i), 5)});
    }
    follower.replicator->onMessage(1, Message{first});
    CHECK_EQ(follower.log.lastSeq(), Sequence{3});
    CHECK_EQ(follower.replicator->lastApplied(), Sequence{3});

    // A leader claiming the entry at seq 3 was written in term 9. It was
    // not: this follower holds a different entry there. Accepting would
    // splice two histories together and produce a book that never
    // existed on any node.
    const auto before = follower.replicator->stats().logMatchRejections;
    net::AppendEntries wrongTerm;
    wrongTerm.term = 9;
    wrongTerm.leaderId = 3;
    wrongTerm.prevSeq = 3;
    wrongTerm.prevTerm = 9;  // we have term 2 there
    wrongTerm.commitSeq = 3;
    wrongTerm.entries.push_back(net::LogRecord{4, 9, support::limit(99, Side::Sell, 500, 5)});
    follower.replicator->setTerm(9);
    follower.replicator->onMessage(3, Message{wrongTerm});

    CHECK_EQ(follower.replicator->stats().logMatchRejections, before + 1);
    // Rejected, so nothing was appended.
    CHECK_EQ(follower.log.lastSeq(), Sequence{3});

    // The same message with the correct prevTerm is accepted.
    net::AppendEntries rightTerm = wrongTerm;
    rightTerm.prevTerm = 2;
    follower.replicator->onMessage(3, Message{rightTerm});
    CHECK_EQ(follower.log.lastSeq(), Sequence{4});
}

// RAFT FIGURE 8. A leader may not commit an entry from a PREVIOUS term
// just because a majority now holds it.
//
// This is the subtlest rule in the project and it was completely
// unguarded: removing the check changed no test result. It went unnoticed
// because reaching the situation naturally needs a specific multi-term
// sequence of partial replications and elections, so it is constructed
// directly here instead.
void testLeaderWillNotCommitAPreviousTermsEntry() {
    Node leader(1, Role::Primary, ReplicationMode::Asynchronous);
    leader.replicator->setClusterSize(3);

    // Three entries left over from term 2, as a previous leader wrote
    // them and this node inherited them.
    for (int i = 1; i <= 3; ++i) {
        leader.log.append(support::limit(static_cast<OrderId>(i), Side::Buy, 100 + i, 5), 2);
    }
    // We are now leading term 5.
    leader.replicator->setTerm(5);
    CHECK_EQ(leader.replicator->commitIndex(), Sequence{0});

    // Both followers report holding all three. That is a majority of
    // three, so the naive rule would commit them.
    for (NodeId peer : {NodeId{2}, NodeId{3}}) {
        net::AppendResponse ack;
        ack.term = 5;
        ack.nodeId = peer;
        ack.lastLogSeq = 3;
        ack.ok = true;
        ack.lastApplied = 0;
        leader.replicator->onMessage(peer, Message{ack});
    }

    // And it must NOT. A majority holding an old entry does not make it
    // safe, because a future leader could still be elected without it.
    // Raft's Figure 8 is the five-node schedule where committing here
    // lets a committed entry be overwritten later.
    CHECK_EQ(leader.replicator->commitIndex(), Sequence{0});
    CHECK_EQ(leader.replicator->lastApplied(), Sequence{0});

    // Now the leader appends an entry in its OWN term and a majority
    // takes it. That entry is safe to commit, and committing it carries
    // the earlier ones with it. This is how old entries become committed:
    // never directly, always in the wake of a current-term entry.
    leader.log.append(support::limit(4, Side::Sell, 200, 5), 5);
    for (NodeId peer : {NodeId{2}, NodeId{3}}) {
        net::AppendResponse ack;
        ack.term = 5;
        ack.nodeId = peer;
        ack.lastLogSeq = 4;
        ack.ok = true;
        ack.lastApplied = 0;
        leader.replicator->onMessage(peer, Message{ack});
    }
    CHECK_EQ(leader.replicator->commitIndex(), Sequence{4});
    CHECK_EQ(leader.replicator->lastApplied(), Sequence{4});
    CHECK_EQ(leader.engine.book().orderCount(), std::size_t{4});
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
    testLogMatchingRejectsAWrongTerm();
    testLeaderWillNotCommitAPreviousTermsEntry();
    testCatchUpAfterDisconnect();
    testLogCompaction();
}

}  // namespace

TEST_MAIN("test_replication")
