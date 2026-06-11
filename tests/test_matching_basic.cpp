// The plain crossing cases: a resting order, an aggressor that takes it
// whole, and the events that must come out in order.
#include "sententia/engine.hpp"

#include "harness.hpp"
#include "support.hpp"

using namespace sententia;
using namespace support;

namespace {

void run() {
    MatchingEngine engine(kInstrument);

    // A lone limit order rests and moves the top of book.
    EventList first = engine.apply(limit(1, Side::Sell, 100, 10));
    CHECK_EQ(countOf<OrderAccepted>(first), std::size_t{1});
    CHECK_EQ(countOf<OrderResting>(first), std::size_t{1});
    CHECK_EQ(countOf<Trade>(first), std::size_t{0});
    CHECK_EQ(countOf<TopOfBookChanged>(first), std::size_t{1});
    CHECK_EQ(nth<TopOfBookChanged>(first, 0).askPrice, Price{100});

    // A marketable buy takes it entirely.
    EventList second = engine.apply(limit(2, Side::Buy, 100, 10));
    CHECK_EQ(countOf<Trade>(second), std::size_t{1});
    const Trade& t = nth<Trade>(second, 0);
    CHECK_EQ(t.aggressorId, OrderId{2});
    CHECK_EQ(t.restingId, OrderId{1});
    CHECK_EQ(t.price, Price{100});
    CHECK_EQ(t.quantity, Quantity{10});
    CHECK(t.aggressorSide == Side::Buy);
    // Nothing rests: the aggressor was fully filled.
    CHECK_EQ(countOf<OrderResting>(second), std::size_t{0});
    CHECK(engine.book().empty());

    // Events are ordered accepted, then trade, then top of book.
    const auto lines = render(second);
    CHECK(lines.at(0).find("ACCEPTED") != std::string::npos);
    CHECK(lines.at(1).find("TRADE") != std::string::npos);
    CHECK(lines.at(2).find("TOB") != std::string::npos);

    // Event sequence numbers are strictly increasing across commands.
    CHECK_EQ(header(first.front()).eventSeq, Sequence{1});
    CHECK(header(second.back()).eventSeq > header(first.back()).eventSeq);
    CHECK_EQ(engine.commandSequence(), Sequence{2});

    // A non-marketable order does not trade.
    MatchingEngine quiet(kInstrument);
    quiet.apply(limit(1, Side::Sell, 105, 10));
    EventList third = quiet.apply(limit(2, Side::Buy, 100, 10));
    CHECK_EQ(countOf<Trade>(third), std::size_t{0});
    CHECK_EQ(countOf<OrderResting>(third), std::size_t{1});
    CHECK_EQ(quiet.book().bestBid().value(), Price{100});
    CHECK_EQ(quiet.book().bestAsk().value(), Price{105});

    // A crossing order takes price improvement: it executes at the
    // resting price, not at its own more aggressive limit.
    MatchingEngine improve(kInstrument);
    improve.apply(limit(1, Side::Sell, 100, 10));
    EventList fourth = improve.apply(limit(2, Side::Buy, 110, 10));
    CHECK_EQ(nth<Trade>(fourth, 0).price, Price{100});
}

}  // namespace

TEST_MAIN("test_matching_basic")
