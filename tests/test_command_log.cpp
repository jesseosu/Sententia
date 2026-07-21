// The replicated command log: sequencing, gap refusal, range serving,
// and truncation.
#include "sententia/replication/command_log.hpp"

#include "harness.hpp"
#include "support.hpp"

using namespace sententia;
using namespace sententia::replication;

namespace {

void run() {
    CommandLog log;
    CHECK(log.empty());
    // Sequence numbers start at 1 so that zero can mean "nothing yet".
    CHECK_EQ(log.lastSeq(), Sequence{0});
    CHECK_EQ(log.firstSeq(), Sequence{1});

    CHECK_EQ(log.append(support::limit(1, Side::Buy, 100, 10)), Sequence{1});
    CHECK_EQ(log.append(support::limit(2, Side::Sell, 101, 5)), Sequence{2});
    CHECK_EQ(log.append(support::cancel(1)), Sequence{3});
    CHECK_EQ(log.lastSeq(), Sequence{3});
    CHECK_EQ(log.size(), std::size_t{3});

    CHECK(log.at(1) != nullptr);
    CHECK(log.at(3) != nullptr);
    CHECK(log.at(0) == nullptr);
    CHECK(log.at(4) == nullptr);
    if (log.at(2) != nullptr) {
        CHECK_EQ(log.at(2)->seq, Sequence{2});
    }

    // Ranges are clamped to what exists rather than erroring.
    CHECK_EQ(log.range(1, 10).size(), std::size_t{3});
    CHECK_EQ(log.range(2, 1).size(), std::size_t{1});
    CHECK_EQ(log.range(4, 10).size(), std::size_t{0});
    CHECK_EQ(log.range(1, 0).size(), std::size_t{0});
    if (!log.range(2, 5).empty()) {
        CHECK_EQ(log.range(2, 5).front().seq, Sequence{2});
    }

    // A backup appending at an explicit sequence must be exactly in
    // order. Accepting a gap here would break the determinism contract
    // silently, which is the one failure this whole design cannot
    // tolerate, so it is refused rather than repaired.
    {
        CommandLog backup;
        CHECK(backup.appendAt(1, support::limit(1, Side::Buy, 100, 10)));
        CHECK(!backup.appendAt(3, support::limit(3, Side::Buy, 100, 10)));  // gap
        CHECK(!backup.appendAt(1, support::limit(9, Side::Buy, 100, 10)));  // replay
        CHECK_EQ(backup.lastSeq(), Sequence{1});
        CHECK(backup.appendAt(2, support::limit(2, Side::Buy, 100, 10)));
        CHECK_EQ(backup.lastSeq(), Sequence{2});
    }

    // Truncation drops acknowledged entries and moves the window.
    log.truncateThrough(2);
    CHECK_EQ(log.size(), std::size_t{1});
    CHECK_EQ(log.firstSeq(), Sequence{3});
    CHECK_EQ(log.lastSeq(), Sequence{3});
    CHECK(log.at(1) == nullptr);
    CHECK(log.at(3) != nullptr);

    // A range starting before the retained window returns nothing rather
    // than silently starting later, which would hand back a gap
    // disguised as a valid answer.
    CHECK_EQ(log.range(1, 10).size(), std::size_t{0});
    CHECK(!log.canServeFrom(1));
    CHECK(log.canServeFrom(3));
    // Asking for the next unwritten sequence is fine: it means current.
    CHECK(log.canServeFrom(4));
    CHECK(!log.canServeFrom(5));

    // Truncating everything keeps the sequence counter, so numbering
    // never restarts and a stale backup cannot be silently accepted.
    log.truncateThrough(3);
    CHECK(log.empty());
    CHECK_EQ(log.lastSeq(), Sequence{3});
    CHECK_EQ(log.firstSeq(), Sequence{4});
    CHECK_EQ(log.append(support::cancel(2)), Sequence{4});

    // Bulk sequencing stays gapless.
    {
        CommandLog bulk;
        const auto cmds = support::generateCommands(0x1234ULL, 5000);
        for (std::size_t i = 0; i < cmds.size(); ++i) {
            CHECK_EQ(bulk.append(cmds[i]), Sequence{i + 1});
        }
        CHECK_EQ(bulk.lastSeq(), Sequence{5000});
        const auto batch = bulk.range(100, 512);
        CHECK_EQ(batch.size(), std::size_t{512});
        bool contiguous = true;
        for (std::size_t i = 0; i < batch.size(); ++i) {
            if (batch[i].seq != 100 + i) {
                contiguous = false;
            }
        }
        CHECK(contiguous);
    }
}

}  // namespace

TEST_MAIN("test_command_log")
