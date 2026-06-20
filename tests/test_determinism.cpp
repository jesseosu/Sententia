// The determinism contract.
//
// This is the test that earns Phase 1 the right to start distribution.
// The claim it pins down:
//
//   Given the same starting state and the same ordered command sequence,
//   the engine emits exactly the same events, in the same order, and
//   ends in exactly the same state. Every run. Every process.
//
// If this ever fails, replication in Phase 3 and recovery in Phase 5 are
// unimplementable, because two replicas fed identical input would
// diverge. Nothing downstream is worth debugging until this is green.
#include "sententia/engine.hpp"

#include <string>
#include <vector>

#include "harness.hpp"
#include "support.hpp"

using namespace sententia;
using namespace support;

namespace {

// A small, hand-written sequence whose exact output is written down in
// full. The generated sequences below prove self-consistency; this one
// proves the engine agrees with what a human decided it should do.
const std::vector<Command>& goldenCommands() {
    static const std::vector<Command> cmds = {
        limit(1, Side::Buy, 100, 10),
        limit(2, Side::Buy, 100, 5),
        limit(3, Side::Sell, 101, 8),
        limit(4, Side::Sell, 100, 12),
        cancel(2),
        cancel(2),
        market(5, Side::Buy, 20),
        limit(6, Side::Sell, 99, 4, TimeInForce::ImmediateOrCancel),
    };
    return cmds;
}

const std::vector<std::string>& goldenEvents() {
    static const std::vector<std::string> expected = {
        "seq=1 cmd=1 ACCEPTED id=1 side=BUY type=LIMIT tif=GTC px=100 qty=10",
        "seq=2 cmd=1 RESTING id=1 side=BUY px=100 qty=10",
        "seq=3 cmd=1 TOB bid=100x10 ask=none",
        "seq=4 cmd=2 ACCEPTED id=2 side=BUY type=LIMIT tif=GTC px=100 qty=5",
        "seq=5 cmd=2 RESTING id=2 side=BUY px=100 qty=5",
        "seq=6 cmd=2 TOB bid=100x15 ask=none",
        "seq=7 cmd=3 ACCEPTED id=3 side=SELL type=LIMIT tif=GTC px=101 qty=8",
        "seq=8 cmd=3 RESTING id=3 side=SELL px=101 qty=8",
        "seq=9 cmd=3 TOB bid=100x15 ask=101x8",
        "seq=10 cmd=4 ACCEPTED id=4 side=SELL type=LIMIT tif=GTC px=100 qty=12",
        "seq=11 cmd=4 TRADE aggressor=4 resting=1 side=SELL px=100 qty=10",
        "seq=12 cmd=4 TRADE aggressor=4 resting=2 side=SELL px=100 qty=2",
        "seq=13 cmd=4 TOB bid=100x3 ask=101x8",
        "seq=14 cmd=5 CANCELLED id=2 side=BUY px=100 qty=3 by=CLIENT",
        "seq=15 cmd=5 TOB bid=none ask=101x8",
        "seq=16 cmd=6 CANCEL_REJECTED id=2 reason=UNKNOWN_ORDER_ID",
        "seq=17 cmd=7 ACCEPTED id=5 side=BUY type=MARKET tif=IOC px=0 qty=20",
        "seq=18 cmd=7 TRADE aggressor=5 resting=3 side=BUY px=101 qty=8",
        "seq=19 cmd=7 CANCELLED id=5 side=BUY px=0 qty=12 by=ENGINE",
        "seq=20 cmd=7 TOB bid=none ask=none",
        "seq=21 cmd=8 ACCEPTED id=6 side=SELL type=LIMIT tif=IOC px=99 qty=4",
        "seq=22 cmd=8 CANCELLED id=6 side=SELL px=99 qty=4 by=ENGINE",
    };
    return expected;
}

void checkGolden() {
    MatchingEngine engine(kInstrument);
    const EventList events = runAll(engine, goldenCommands());
    const std::vector<std::string> actual = render(events);
    const std::vector<std::string>& expected = goldenEvents();

    CHECK_EQ(actual.size(), expected.size());
    const std::size_t n = actual.size() < expected.size() ? actual.size() : expected.size();
    for (std::size_t i = 0; i < n; ++i) {
        CHECK_STR_EQ(actual[i], expected[i]);
    }
    // The book is empty at the end of this sequence, which is itself
    // part of the contract.
    CHECK(engine.book().empty());
}

// Same input, many runs, in one process. Catches anything that depends
// on allocator reuse, iteration order, or leftover state.
void checkRepeatedRuns() {
    const std::vector<Command> cmds = generateCommands(0xC0FFEEULL, 20000);

    MatchingEngine reference(kInstrument);
    const EventList referenceEvents = runAll(reference, cmds);
    const std::uint64_t referenceEventHash = hashEvents(referenceEvents);
    const std::uint64_t referenceState = reference.stateChecksum();

    // The stream has to be substantial or the test proves nothing.
    CHECK(referenceEvents.size() > 20000);
    CHECK(countOf<Trade>(referenceEvents) > 1000);

    for (int run = 0; run < 50; ++run) {
        MatchingEngine engine(kInstrument);
        const EventList events = runAll(engine, cmds);
        CHECK_EQ(hashEvents(events), referenceEventHash);
        CHECK_EQ(engine.stateChecksum(), referenceState);
        CHECK_EQ(events.size(), referenceEvents.size());
    }

    // Hashes are a summary; confirm the full streams really are equal,
    // event for event, at least once.
    MatchingEngine again(kInstrument);
    const EventList againEvents = runAll(again, cmds);
    CHECK_EQ(againEvents.size(), referenceEvents.size());
    bool identical = againEvents.size() == referenceEvents.size();
    for (std::size_t i = 0; identical && i < againEvents.size(); ++i) {
        identical = againEvents[i] == referenceEvents[i];
    }
    CHECK(identical);
}

// Replaying a prefix and then continuing must equal replaying the whole
// thing. This is exactly the operation a recovering node performs in
// Phase 5, so it is worth pinning now rather than discovering later.
void checkPrefixReplay() {
    const std::vector<Command> cmds = generateCommands(0xBEEFULL, 5000);

    MatchingEngine whole(kInstrument);
    const EventList wholeEvents = runAll(whole, cmds);

    const std::size_t split = cmds.size() / 3;
    MatchingEngine piecewise(kInstrument);
    EventList piecewiseEvents;
    for (std::size_t i = 0; i < split; ++i) {
        piecewise.apply(cmds[i], piecewiseEvents);
    }
    const std::uint64_t midpoint = piecewise.stateChecksum();
    for (std::size_t i = split; i < cmds.size(); ++i) {
        piecewise.apply(cmds[i], piecewiseEvents);
    }

    CHECK_EQ(hashEvents(piecewiseEvents), hashEvents(wholeEvents));
    CHECK_EQ(piecewise.stateChecksum(), whole.stateChecksum());

    // A second engine driven over the same prefix reaches the same
    // midpoint state, so state after N commands is a pure function of
    // those N commands.
    MatchingEngine prefixOnly(kInstrument);
    EventList discard;
    for (std::size_t i = 0; i < split; ++i) {
        prefixOnly.apply(cmds[i], discard);
    }
    CHECK_EQ(prefixOnly.stateChecksum(), midpoint);
}

// Different input must give different output. Without this the test
// above would also pass on an engine that ignored its input entirely.
void checkSensitivity() {
    std::vector<Command> base = generateCommands(0xC0FFEEULL, 2000);
    MatchingEngine a(kInstrument);
    const std::uint64_t hashA = hashEvents(runAll(a, base));

    // Perturb one quantity by one unit.
    for (Command& c : base) {
        if (auto* n = std::get_if<NewOrder>(&c)) {
            n->quantity += 1;
            break;
        }
    }
    MatchingEngine b(kInstrument);
    const std::uint64_t hashB = hashEvents(runAll(b, base));
    CHECK(hashA != hashB);

    // Reordering two commands changes the outcome too, which is why the
    // *order* of the replicated log matters and not just its contents.
    std::vector<Command> swapped = generateCommands(0xC0FFEEULL, 2000);
    std::swap(swapped[10], swapped[11]);
    MatchingEngine c(kInstrument);
    CHECK(hashEvents(runAll(c, swapped)) != hashA);
}

void run() {
    checkGolden();
    checkRepeatedRuns();
    checkPrefixReplay();
    checkSensitivity();
}

}  // namespace

TEST_MAIN("test_determinism")
