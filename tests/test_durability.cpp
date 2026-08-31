// Durability: the write-ahead log, torn writes, the stable store, and
// snapshot round-trips.
//
// The scenario every test here is really about: the machine loses power
// at the worst possible moment. What survives, what does not, and is the
// difference exactly what was promised to a client.
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

namespace {

// A scratch directory that cleans up after itself.
struct TempDir {
    std::string path;
    explicit TempDir(const char* tag) {
        char buf[] = "/tmp/sententia_test_XXXXXX";
        path = ::mkdtemp(buf) ? std::string(buf) : std::string("/tmp/sententia_fallback");
        path += "_";
        path += tag;
        ::mkdir(path.c_str(), 0755);
    }
    ~TempDir() {
        const std::string cmd = "rm -rf " + path;
        const int rc = std::system(cmd.c_str());
        (void)rc;
    }
    std::string file(const char* name) const { return path + "/" + name; }
};

void testWalRoundTrip() {
    TempDir dir("wal");
    const std::string path = dir.file("a.wal");
    {
        WriteAheadLog wal;
        std::string error;
        CHECK(wal.open(path, SyncPolicy::EveryWrite, error));
        CHECK_EQ(wal.records().size(), std::size_t{0});
        for (std::uint32_t i = 1; i <= 100; ++i) {
            const std::string payload = "record-" + std::to_string(i);
            CHECK(wal.append(i, reinterpret_cast<const Byte*>(payload.data()), payload.size(),
                             error));
        }
        // EveryWrite means one fsync per append. That is the cost of the
        // strongest guarantee, and it is worth seeing the number.
        CHECK_EQ(wal.stats().fsyncs, std::uint64_t{100});
    }
    {
        WriteAheadLog wal;
        std::string error;
        CHECK(wal.open(path, SyncPolicy::EveryWrite, error));
        CHECK_EQ(wal.records().size(), std::size_t{100});
        CHECK(!wal.stats().tailWasTorn);
        bool allMatch = true;
        for (std::size_t i = 0; i < wal.records().size(); ++i) {
            const std::string expected = "record-" + std::to_string(i + 1);
            const auto& r = wal.records()[i];
            if (r.seq != i + 1 || std::string(r.payload.begin(), r.payload.end()) != expected) {
                allMatch = false;
            }
        }
        CHECK(allMatch);
    }
}

void testBatchedSyncsLessOften() {
    TempDir dir("batched");
    WriteAheadLog wal;
    std::string error;
    CHECK(wal.open(dir.file("b.wal"), SyncPolicy::Batched, error));
    wal.setBatchSize(10);
    for (std::uint32_t i = 1; i <= 100; ++i) {
        const std::string p = "x";
        CHECK(wal.append(i, reinterpret_cast<const Byte*>(p.data()), p.size(), error));
    }
    // Ten syncs instead of a hundred. The tradeoff made concrete: a
    // crash can lose up to nine acknowledged commands in exchange for a
    // tenth of the syscalls.
    CHECK_EQ(wal.stats().fsyncs, std::uint64_t{10});
}

// The important one. A crash mid-write leaves a half-record, and reading
// it back naively gives garbage that looks like data.
void testTornTailIsDiscarded() {
    TempDir dir("torn");
    const std::string path = dir.file("t.wal");
    {
        WriteAheadLog wal;
        std::string error;
        CHECK(wal.open(path, SyncPolicy::EveryWrite, error));
        for (std::uint32_t i = 1; i <= 10; ++i) {
            const std::string p = "payload-" + std::to_string(i);
            CHECK(wal.append(i, reinterpret_cast<const Byte*>(p.data()), p.size(), error));
        }
    }

    // Simulate a crash partway through an eleventh record by appending
    // a header with no payload behind it.
    {
        std::FILE* f = std::fopen(path.c_str(), "ab");
        CHECK(f != nullptr);
        if (f != nullptr) {
            unsigned char header[16] = {};
            header[0] = 0x5F;
            header[1] = 0x57;
            header[2] = 0x41;
            header[3] = 0x4C;  // magic
            header[4] = 11;    // seq
            header[8] = 99;    // length
            std::fwrite(header, 1, sizeof(header), f);
            std::fclose(f);
        }
    }

    {
        WriteAheadLog wal;
        std::string error;
        CHECK(wal.open(path, SyncPolicy::EveryWrite, error));
        // The ten good records survive; the eleventh never happened.
        CHECK_EQ(wal.records().size(), std::size_t{10});
        CHECK(wal.stats().tailWasTorn);
        CHECK_EQ(wal.stats().truncatedTailBytes, std::uint64_t{16});
        // And the file is now clean, so the next append lands correctly
        // rather than after a hole.
        CHECK(wal.append(11, reinterpret_cast<const Byte*>("ok"), 2, error));
    }
    {
        WriteAheadLog wal;
        std::string error;
        CHECK(wal.open(path, SyncPolicy::EveryWrite, error));
        CHECK_EQ(wal.records().size(), std::size_t{11});
        CHECK(!wal.stats().tailWasTorn);
    }
}

void testCorruptedPayloadIsDetected() {
    TempDir dir("corrupt");
    const std::string path = dir.file("c.wal");
    {
        WriteAheadLog wal;
        std::string error;
        CHECK(wal.open(path, SyncPolicy::EveryWrite, error));
        for (std::uint32_t i = 1; i <= 5; ++i) {
            const std::string p = "aaaaaaaaaa";
            CHECK(wal.append(i, reinterpret_cast<const Byte*>(p.data()), p.size(), error));
        }
    }
    // Flip a byte inside the third record's payload. The CRC exists for
    // exactly this: a disk that returned something other than what was
    // written must not be mistaken for valid data.
    {
        std::FILE* f = std::fopen(path.c_str(), "r+b");
        CHECK(f != nullptr);
        if (f != nullptr) {
            const long recordSize = 16 + 10;
            std::fseek(f, recordSize * 2 + 16 + 3, SEEK_SET);
            std::fputc('Z', f);
            std::fclose(f);
        }
    }
    {
        WriteAheadLog wal;
        std::string error;
        CHECK(wal.open(path, SyncPolicy::EveryWrite, error));
        // Everything from the bad record on is discarded, not just the
        // bad one: after a corrupt record there is no way to trust what
        // follows it.
        CHECK_EQ(wal.records().size(), std::size_t{2});
        CHECK(wal.stats().tailWasTorn);
    }
}

void testStableStoreSurvivesRestart() {
    TempDir dir("stable");
    const std::string path = dir.file("state");
    {
        StableStore store;
        std::string error;
        CHECK(store.open(path, error));
        // A node that has never run: term 0, no vote.
        CHECK_EQ(store.state().currentTerm, std::uint64_t{0});
        CHECK_EQ(store.state().votedFor, NodeId{0});
        CHECK(!store.stats().loadedExisting);

        CHECK(store.save(StableState{7, 3}, error));
    }
    {
        StableStore store;
        std::string error;
        CHECK(store.open(path, error));
        // This is the Phase 4 correctness hole closed. A node that
        // restarts remembers it already voted for node 3 in term 7, so
        // it cannot be talked into voting again in the same term and
        // producing two leaders.
        CHECK_EQ(store.state().currentTerm, std::uint64_t{7});
        CHECK_EQ(store.state().votedFor, NodeId{3});
        CHECK(store.stats().loadedExisting);
    }
}

void testSnapshotRoundTripsExactly() {
    // A snapshot that restores to a DIFFERENT book than it recorded is
    // worse than no snapshot, because it looks like it worked. So the
    // property under test is checksum equality, not rough similarity.
    MatchingEngine engine(support::kInstrument);
    const auto cmds = support::generateCommands(0x5A5AULL, 4000);
    EventList sink;
    for (const Command& c : cmds) {
        sink.clear();
        engine.apply(c, sink);
    }
    CHECK(engine.book().orderCount() > 50);

    const auto snap = engine.snapshot();
    const std::uint64_t original = engine.stateChecksum();

    MatchingEngine restored(support::kInstrument);
    CHECK(restored.restore(snap));
    CHECK_EQ(restored.stateChecksum(), original);
    CHECK_EQ(restored.book().checksum(), engine.book().checksum());
    CHECK_EQ(restored.commandSequence(), engine.commandSequence());

    // And it keeps behaving identically afterwards, which is the part
    // that would break if the arrival counter were left out: time
    // priority for every future order would be wrong.
    const auto more = support::generateCommands(0xB00BULL, 500);
    for (const Command& c : more) {
        sink.clear();
        engine.apply(c, sink);
        sink.clear();
        restored.apply(c, sink);
    }
    CHECK_EQ(restored.stateChecksum(), engine.stateChecksum());

    // Through the on-disk encoding too.
    SnapshotRecord record;
    record.lastIncludedSeq = 4000;
    record.lastIncludedTerm = 3;
    record.engine = snap;
    record.stateChecksum = original;
    const auto bytes = encodeSnapshot(record);
    const auto decoded = decodeSnapshot(bytes.data(), bytes.size());
    CHECK(decoded.has_value());
    if (decoded.has_value()) {
        MatchingEngine fromDisk(support::kInstrument);
        CHECK(fromDisk.restore(decoded->engine));
        CHECK_EQ(fromDisk.stateChecksum(), original);
        CHECK_EQ(decoded->lastIncludedSeq, std::uint64_t{4000});
        CHECK_EQ(decoded->lastIncludedTerm, std::uint64_t{3});
    }
}

void run() {
    testWalRoundTrip();
    testBatchedSyncsLessOften();
    testTornTailIsDiscarded();
    testCorruptedPayloadIsDetected();
    testStableStoreSurvivesRestart();
    testSnapshotRoundTripsExactly();
}

}  // namespace

TEST_MAIN("test_durability")
