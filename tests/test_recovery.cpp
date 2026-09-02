// Recovery: a node dies, restarts, and comes back to exactly the state
// it was in, provably.
//
// The claim: recovery is not a special code path that reconstructs state
// some other way. It is the ordinary apply loop, fed from disk instead
// of from a socket. That only works because the engine is deterministic,
// which is Phase 1 paying off for the fourth time.
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "harness.hpp"
#include "sententia/storage/durable_state.hpp"
#include "support.hpp"

using namespace sententia;
using namespace sententia::storage;
using namespace sententia::replication;

namespace {

struct TempDir {
    std::string path;
    explicit TempDir(const char* tag) {
        char buf[] = "/tmp/sententia_rec_XXXXXX";
        path = ::mkdtemp(buf) ? std::string(buf) : std::string("/tmp/sententia_rec_fallback");
        path += "_";
        path += tag;
        ::mkdir(path.c_str(), 0755);
    }
    ~TempDir() {
        const int rc = std::system(("rm -rf " + path).c_str());
        (void)rc;
    }
};

// Runs a command stream through an engine while journalling it, exactly
// as a leader would: write to the log first, then apply.
struct Journal {
    MatchingEngine engine{support::kInstrument};
    CommandLog log;
    DurableState store;

    bool open(const std::string& dir, SyncPolicy policy, std::string& error) {
        return store.open(dir, policy, error);
    }

    bool applyAll(const std::vector<Command>& cmds, std::uint64_t term, std::string& error) {
        EventList sink;
        for (const Command& c : cmds) {
            const Sequence seq = log.append(c, term);
            const LogEntry* entry = log.at(seq);
            // WRITE AHEAD: the entry reaches disk before it is applied.
            // Reversing these two lines is how a system tells a client
            // something happened and then forgets it.
            if (!store.appendEntry(*entry, error)) {
                return false;
            }
            sink.clear();
            engine.apply(c, sink);
        }
        return true;
    }
};

void testRestartReplaysToTheSameState() {
    TempDir dir("replay");
    const auto cmds = support::generateCommands(0x1111ULL, 3000);
    std::uint64_t original = 0;
    std::uint64_t originalBook = 0;

    {
        Journal j;
        std::string error;
        CHECK(j.open(dir.path, SyncPolicy::EveryWrite, error));
        CHECK(j.applyAll(cmds, 1, error));
        original = j.engine.stateChecksum();
        originalBook = j.engine.book().checksum();
        CHECK(j.engine.book().orderCount() > 50);
    }

    // The process is gone. Everything was in memory except the log.
    {
        MatchingEngine engine(support::kInstrument);
        CommandLog log;
        DurableState store;
        RecoveryReport report;
        std::string error;
        CHECK(store.open(dir.path, SyncPolicy::EveryWrite, error));
        CHECK(store.recover(engine, log, report, error));

        CHECK(!report.hadSnapshot);
        CHECK_EQ(report.entriesReplayed, std::uint64_t{3000});
        CHECK_EQ(report.lastLogSeq, std::uint64_t{3000});
        CHECK(!report.tornTailDiscarded);

        // The whole point: byte-identical, not approximately right.
        CHECK_EQ(engine.stateChecksum(), original);
        CHECK_EQ(engine.book().checksum(), originalBook);
        CHECK_EQ(log.lastSeq(), Sequence{3000});
        // Terms survive the round trip, which log matching depends on.
        CHECK(log.termAt(3000).has_value());
        if (log.termAt(3000).has_value()) {
            CHECK_EQ(log.termAt(3000).value(), std::uint64_t{1});
        }
    }
}

void testSnapshotBoundsRecovery() {
    TempDir dir("snap");
    const auto first = support::generateCommands(0x2222ULL, 2000);
    const auto second = support::generateCommands(0x3333ULL, 300);
    std::uint64_t finalChecksum = 0;

    {
        Journal j;
        std::string error;
        CHECK(j.open(dir.path, SyncPolicy::EveryWrite, error));
        CHECK(j.applyAll(first, 1, error));

        // Snapshot at 2000, which makes the first 2000 log entries
        // redundant. Recovery no longer replays them.
        CHECK(j.store.takeSnapshot(j.engine, j.log.lastSeq(), 1, error));
        j.log.setSnapshotBoundary(j.log.lastSeq(), 1);

        CHECK(j.applyAll(second, 1, error));
        finalChecksum = j.engine.stateChecksum();
    }

    {
        MatchingEngine engine(support::kInstrument);
        CommandLog log;
        DurableState store;
        RecoveryReport report;
        std::string error;
        CHECK(store.open(dir.path, SyncPolicy::EveryWrite, error));
        CHECK(store.recover(engine, log, report, error));

        CHECK(report.hadSnapshot);
        CHECK_EQ(report.snapshotSeq, std::uint64_t{2000});
        // Only the 300 commands since the snapshot were replayed, not
        // all 2300. That is what bounds recovery time: it becomes a
        // function of time since the last snapshot, not of total
        // history, and total history only ever grows.
        CHECK_EQ(report.entriesReplayed, std::uint64_t{300});
        CHECK_EQ(engine.stateChecksum(), finalChecksum);
        CHECK_EQ(log.lastSeq(), Sequence{2300});
    }
}

void testSnapshotEntriesAreNotAppliedTwice() {
    // The classic snapshot-plus-log bug: replay entries the snapshot
    // already contains, and the book is silently wrong rather than
    // obviously broken.
    //
    // For this to be a real test the log must OVERLAP the snapshot: the
    // snapshot is taken at a point BEHIND the log head, so records at
    // and before the boundary are still on disk and recovery has to skip
    // exactly the right ones. The first version of this test snapshotted
    // at the log head, which left no overlap at all, so it could not
    // have detected an off-by-one at the boundary. It was checked
    // against a deliberate one, and did not notice.
    TempDir dir("dup");
    const auto cmds = support::generateCommands(0x4444ULL, 1500);
    std::uint64_t expected = 0;

    {
        Journal j;
        std::string error;
        CHECK(j.open(dir.path, SyncPolicy::EveryWrite, error));
        CHECK(j.applyAll(cmds, 1, error));
        expected = j.engine.stateChecksum();

        // Snapshot 500 entries BEHIND the head. A real system does this
        // constantly: snapshotting is asynchronous and the log keeps
        // moving while it happens.
        MatchingEngine asOf1000(support::kInstrument);
        EventList sink;
        for (std::size_t i = 0; i < 1000; ++i) {
            sink.clear();
            asOf1000.apply(cmds[i], sink);
        }
        CHECK(j.store.takeSnapshot(asOf1000, 1000, 1, error));
    }
    {
        MatchingEngine engine(support::kInstrument);
        CommandLog log;
        DurableState store;
        RecoveryReport report;
        std::string error;
        CHECK(store.open(dir.path, SyncPolicy::EveryWrite, error));
        CHECK(store.recover(engine, log, report, error));

        CHECK(report.hadSnapshot);
        CHECK_EQ(report.snapshotSeq, std::uint64_t{1000});
        // Exactly the 500 after the boundary. Not 501, which an
        // off-by-one would give, and not 1500.
        CHECK_EQ(report.entriesReplayed, std::uint64_t{500});
        CHECK_EQ(log.lastSeq(), Sequence{1500});
        CHECK_EQ(engine.stateChecksum(), expected);
    }
}

// The overlap case that actually matters: a crash BETWEEN writing the
// snapshot and truncating the log.
//
// takeSnapshot writes the snapshot first and truncates second, on
// purpose: the other order loses data if the process dies in between.
// But that ordering means a crash in the gap leaves a snapshot covering
// a prefix AND a log that still contains that whole prefix. Recovery
// then sees every entry twice over, once inside the snapshot and once in
// the log, and must apply exactly the ones after the boundary.
//
// This is the only arrangement in which the skip is load-bearing. An
// earlier version of this test snapshotted at the log head, so no
// overlap existed, and a deliberate off-by-one at the boundary went
// completely unnoticed.
void testCrashBetweenSnapshotAndTruncationDoesNotDoubleApply() {
    TempDir dir("gap");
    const auto cmds = support::generateCommands(0x7777ULL, 1500);
    std::uint64_t expected = 0;

    {
        Journal j;
        std::string error;
        CHECK(j.open(dir.path, SyncPolicy::EveryWrite, error));
        CHECK(j.applyAll(cmds, 1, error));
        expected = j.engine.stateChecksum();

        // Build the state as of entry 1000 and write the snapshot file
        // DIRECTLY, without truncating the log. That is precisely the
        // on-disk state a crash in the gap leaves behind.
        MatchingEngine asOf1000(support::kInstrument);
        EventList sink;
        for (std::size_t i = 0; i < 1000; ++i) {
            sink.clear();
            asOf1000.apply(cmds[i], sink);
        }
        SnapshotRecord snap;
        snap.lastIncludedSeq = 1000;
        snap.lastIncludedTerm = 1;
        snap.engine = asOf1000.snapshot();
        snap.stateChecksum = asOf1000.stateChecksum();
        CHECK(writeSnapshotFile(dir.path + "/snapshot", snap, error));
    }

    {
        MatchingEngine engine(support::kInstrument);
        CommandLog log;
        DurableState store;
        RecoveryReport report;
        std::string error;
        CHECK(store.open(dir.path, SyncPolicy::EveryWrite, error));
        CHECK(store.recover(engine, log, report, error));

        CHECK(report.hadSnapshot);
        CHECK_EQ(report.snapshotSeq, std::uint64_t{1000});
        // The log still holds all 1500. Exactly 500 get applied: the
        // ones after the boundary. An off-by-one here gives 501 and a
        // book that is quietly wrong.
        CHECK_EQ(report.entriesReplayed, std::uint64_t{500});
        CHECK_EQ(log.lastSeq(), Sequence{1500});
        CHECK_EQ(engine.stateChecksum(), expected);
    }
}

void testRecoveryAfterATornWrite() {
    // A crash between writing a record's header and its payload. The
    // half-record is discarded, and the recovered state is exactly the
    // state implied by the records that DID complete. Nothing that was
    // acknowledged is lost, because acknowledgement happens after the
    // flush.
    TempDir dir("torn");
    const auto cmds = support::generateCommands(0x5555ULL, 500);
    std::uint64_t at500 = 0;
    {
        Journal j;
        std::string error;
        CHECK(j.open(dir.path, SyncPolicy::EveryWrite, error));
        CHECK(j.applyAll(cmds, 1, error));
        at500 = j.engine.stateChecksum();
    }
    // Append a header with no payload, as a crash would leave.
    {
        std::FILE* f = std::fopen((dir.path + "/log.wal").c_str(), "ab");
        CHECK(f != nullptr);
        if (f != nullptr) {
            unsigned char header[16] = {};
            header[0] = 0x5F;
            header[1] = 0x57;
            header[2] = 0x41;
            header[3] = 0x4C;
            header[4] = 0xF5;
            header[5] = 0x01;
            header[8] = 40;
            std::fwrite(header, 1, sizeof(header), f);
            std::fclose(f);
        }
    }
    {
        MatchingEngine engine(support::kInstrument);
        CommandLog log;
        DurableState store;
        RecoveryReport report;
        std::string error;
        CHECK(store.open(dir.path, SyncPolicy::EveryWrite, error));
        CHECK(store.recover(engine, log, report, error));
        CHECK(report.tornTailDiscarded);
        CHECK_EQ(report.entriesReplayed, std::uint64_t{500});
        CHECK_EQ(engine.stateChecksum(), at500);
    }
}

void testVoteSurvivesRestart() {
    // The Phase 4 correctness hole, closed. A node that voted, crashed
    // and restarted must remember the vote, or it can vote twice in one
    // term and let two candidates each reach a majority.
    TempDir dir("vote");
    {
        DurableState store;
        std::string error;
        CHECK(store.open(dir.path, SyncPolicy::EveryWrite, error));
        CHECK_EQ(store.stable().currentTerm, std::uint64_t{0});
        CHECK(store.saveVote(9, 2, error));
    }
    {
        DurableState store;
        std::string error;
        CHECK(store.open(dir.path, SyncPolicy::EveryWrite, error));
        CHECK_EQ(store.stable().currentTerm, std::uint64_t{9});
        CHECK_EQ(store.stable().votedFor, NodeId{2});

        MatchingEngine engine(support::kInstrument);
        CommandLog log;
        RecoveryReport report;
        CHECK(store.recover(engine, log, report, error));
        CHECK_EQ(report.currentTerm, std::uint64_t{9});
        CHECK_EQ(report.votedFor, NodeId{2});
    }
}

void testRecoveryIsRepeatable() {
    // Recovering twice from the same files must give the same answer.
    // If recovery were not deterministic, a node that restarted twice
    // could end up in two different states from one set of bytes.
    TempDir dir("repeat");
    const auto cmds = support::generateCommands(0x6666ULL, 1200);
    {
        Journal j;
        std::string error;
        CHECK(j.open(dir.path, SyncPolicy::EveryWrite, error));
        CHECK(j.applyAll(cmds, 2, error));
    }
    std::uint64_t firstAnswer = 0;
    for (int attempt = 0; attempt < 5; ++attempt) {
        MatchingEngine engine(support::kInstrument);
        CommandLog log;
        DurableState store;
        RecoveryReport report;
        std::string error;
        CHECK(store.open(dir.path, SyncPolicy::EveryWrite, error));
        CHECK(store.recover(engine, log, report, error));
        if (attempt == 0) {
            firstAnswer = engine.stateChecksum();
        } else {
            CHECK_EQ(engine.stateChecksum(), firstAnswer);
        }
        CHECK_EQ(report.entriesReplayed, std::uint64_t{1200});
    }
}

// CLUSTER RECOVERY: a leader dies mid-stream, restarts from disk, and
// the cluster ends up in one consistent state with nothing committed
// lost and nothing applied twice.
//
// This is the phase's headline claim, so it is worth being precise about
// what is and is not guaranteed.
//
// The commit rule is what makes it safe. An entry is committed only once
// a MAJORITY holds it. So anything a client was told succeeded is on a
// majority of disks, and the election restriction means no node missing
// it can ever win an election. Entries the leader had accepted but not
// committed may be lost, and that is correct: nobody was ever told they
// succeeded.
void testLeaderCrashLosesNothingCommitted() {
    TempDir dirA("clusterA");
    const auto cmds = support::generateCommands(0x8888ULL, 1200);

    std::uint64_t committedChecksum = 0;
    Sequence committedThrough = 0;

    {
        // A leader journals and applies, with a follower keeping up.
        // Modelled directly rather than over sockets so the crash point
        // is exact instead of approximate.
        Journal leader;
        std::string error;
        CHECK(leader.open(dirA.path, SyncPolicy::EveryWrite, error));

        MatchingEngine followerEngine(support::kInstrument);
        CommandLog followerLog;
        EventList sink;

        for (std::size_t i = 0; i < cmds.size(); ++i) {
            const Sequence seq = leader.log.append(cmds[i], 1);
            const LogEntry* entry = leader.log.at(seq);
            // Write ahead, on the leader.
            CHECK(leader.store.appendEntry(*entry, error));

            // The follower receives and journals it too. Past command
            // 900 the follower stops keeping up, so those entries exist
            // on the leader alone and are therefore NOT committed.
            if (i < 900) {
                CHECK(followerLog.appendAt(seq, 1, cmds[i]));
                sink.clear();
                followerEngine.apply(cmds[i], sink);
                // A majority of two holds it, so it is committed and the
                // leader may apply it.
                sink.clear();
                leader.engine.apply(cmds[i], sink);
                committedThrough = seq;
            }
        }
        committedChecksum = leader.engine.stateChecksum();

        // Both nodes agree on everything committed.
        CHECK_EQ(followerEngine.stateChecksum(), committedChecksum);
        CHECK_EQ(committedThrough, Sequence{900});
        // The leader's LOG runs ahead of what is committed, which is
        // normal and is exactly the state a crash exposes.
        CHECK_EQ(leader.log.lastSeq(), Sequence{1200});
    }

    // The leader process dies here and restarts from its own disk.
    {
        MatchingEngine engine(support::kInstrument);
        CommandLog log;
        DurableState store;
        RecoveryReport report;
        std::string error;
        CHECK(store.open(dirA.path, SyncPolicy::EveryWrite, error));
        CHECK(store.recover(engine, log, report, error));

        // Everything it had written is back, including the 300 entries
        // that were never committed. Recovery restores the LOG; deciding
        // which of it is committed is the cluster's job, not the disk's.
        CHECK_EQ(report.entriesReplayed, std::uint64_t{1200});
        CHECK_EQ(log.lastSeq(), Sequence{1200});

        // And critically: replaying only the committed prefix reproduces
        // exactly the state both nodes agreed on before the crash.
        // Nothing committed was lost.
        MatchingEngine committedOnly(support::kInstrument);
        EventList sink;
        for (Sequence seq = 1; seq <= committedThrough; ++seq) {
            const LogEntry* entry = log.at(seq);
            CHECK(entry != nullptr);
            if (entry != nullptr) {
                sink.clear();
                committedOnly.apply(entry->command, sink);
            }
        }
        CHECK_EQ(committedOnly.stateChecksum(), committedChecksum);

        // Terms survived, so the recovered node can still take part in
        // log matching rather than being unable to prove what it holds.
        for (Sequence seq : {Sequence{1}, Sequence{900}, Sequence{1200}}) {
            const auto t = log.termAt(seq);
            CHECK(t.has_value());
            if (t.has_value()) {
                CHECK_EQ(t.value(), std::uint64_t{1});
            }
        }
    }
}

// A follower that fell too far behind to catch up by replay is given a
// snapshot instead, and lands in exactly the right state.
void testFollowerTooFarBehindInstallsASnapshot() {
    TempDir leaderDir("leadSnap");
    TempDir followerDir("followSnap");
    const auto cmds = support::generateCommands(0x9999ULL, 2000);

    MatchingEngine leaderEngine(support::kInstrument);
    CommandLog leaderLog;
    DurableState leaderStore;
    std::string error;
    CHECK(leaderStore.open(leaderDir.path, SyncPolicy::Never, error));

    EventList sink;
    for (const Command& c : cmds) {
        const Sequence seq = leaderLog.append(c, 4);
        CHECK(leaderStore.appendEntry(*leaderLog.at(seq), error));
        sink.clear();
        leaderEngine.apply(c, sink);
    }
    // The leader compacts, so the early entries a lagging follower would
    // need are simply gone.
    CHECK(leaderStore.takeSnapshot(leaderEngine, leaderLog.lastSeq(), 4, error));
    leaderLog.truncateThrough(leaderLog.lastSeq());
    leaderLog.setSnapshotBoundary(leaderLog.lastSeq(), 4);
    CHECK(!leaderLog.canServeFrom(5));

    // A brand new follower with nothing. Replay is impossible, so the
    // leader ships state instead of commands. This is the one place the
    // project ever sends state, and it exists precisely because the log
    // it would otherwise send no longer exists.
    MatchingEngine followerEngine(support::kInstrument);
    CommandLog followerLog;
    DurableState followerStore;
    CHECK(followerStore.open(followerDir.path, SyncPolicy::Never, error));
    CHECK(leaderStore.snapshot().has_value());
    if (leaderStore.snapshot().has_value()) {
        CHECK(followerStore.installSnapshot(leaderStore.snapshot().value(), followerEngine,
                                            followerLog, error));
    }

    CHECK_EQ(followerEngine.stateChecksum(), leaderEngine.stateChecksum());
    CHECK_EQ(followerLog.snapshotSeq(), Sequence{2000});

    // And it can continue normally from there.
    const auto more = support::generateCommands(0xAAAAULL, 100);
    for (const Command& c : more) {
        const Sequence seq = leaderLog.append(c, 4);
        CHECK(followerLog.appendAt(seq, 4, c));
        sink.clear();
        leaderEngine.apply(c, sink);
        sink.clear();
        followerEngine.apply(c, sink);
    }
    CHECK_EQ(followerEngine.stateChecksum(), leaderEngine.stateChecksum());

    // The follower survives its own restart from the installed snapshot.
    {
        MatchingEngine restarted(support::kInstrument);
        CommandLog restartedLog;
        DurableState restartedStore;
        RecoveryReport report;
        CHECK(restartedStore.open(followerDir.path, SyncPolicy::Never, error));
        CHECK(restartedStore.recover(restarted, restartedLog, report, error));
        CHECK(report.hadSnapshot);
        CHECK_EQ(report.snapshotSeq, std::uint64_t{2000});
    }
}

void run() {
    testRestartReplaysToTheSameState();
    testSnapshotBoundsRecovery();
    testSnapshotEntriesAreNotAppliedTwice();
    testCrashBetweenSnapshotAndTruncationDoesNotDoubleApply();
    testRecoveryAfterATornWrite();
    testVoteSurvivesRestart();
    testRecoveryIsRepeatable();
    testLeaderCrashLosesNothingCommitted();
    testFollowerTooFarBehindInstallsASnapshot();
}

}  // namespace

TEST_MAIN("test_recovery")
