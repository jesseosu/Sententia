// Validation and the awkward corners: empty book, bad input, id reuse,
// self-crossing, and extreme quantities.
#include "sententia/engine.hpp"

#include <limits>

#include "harness.hpp"
#include "support.hpp"

using namespace sententia;
using namespace support;

namespace {

void run() {
    MatchingEngine engine(kInstrument);

    // An empty book absorbs a resting order without trading.
    EventList out = engine.apply(limit(1, Side::Buy, 100, 10));
    CHECK_EQ(countOf<Trade>(out), std::size_t{0});
    CHECK_EQ(countOf<OrderResting>(out), std::size_t{1});

    // Zero quantity is rejected.
    EventList z = engine.apply(limit(2, Side::Buy, 100, 0));
    CHECK_EQ(countOf<OrderRejected>(z), std::size_t{1});
    CHECK(nth<OrderRejected>(z, 0).reason == RejectReason::ZeroQuantity);

    // A non-positive limit price is rejected. Prices are ticks, and a
    // zero or negative tick has no meaning in this model.
    EventList p = engine.apply(limit(3, Side::Buy, 0, 10));
    CHECK(nth<OrderRejected>(p, 0).reason == RejectReason::NonPositivePrice);
    EventList n = engine.apply(limit(4, Side::Buy, -5, 10));
    CHECK(nth<OrderRejected>(n, 0).reason == RejectReason::NonPositivePrice);

    // Wrong instrument is rejected before anything else is considered,
    // so a command that is wrong in two ways reports the instrument.
    NewOrder foreign = limit(5, Side::Buy, 100, 0);
    foreign.instrument = kInstrument + 1;
    EventList w = engine.apply(foreign);
    CHECK(nth<OrderRejected>(w, 0).reason == RejectReason::WrongInstrument);

    // A duplicate id is rejected while the original is live.
    EventList d = engine.apply(limit(1, Side::Buy, 101, 5));
    CHECK(nth<OrderRejected>(d, 0).reason == RejectReason::DuplicateOrderId);
    // No rejected command changes the book.
    CHECK_EQ(engine.book().orderCount(), std::size_t{1});

    // Once the original is gone the id is free again. Ids identify live
    // orders, not history.
    engine.apply(cancel(1));
    EventList reuse = engine.apply(limit(1, Side::Buy, 101, 5));
    CHECK_EQ(countOf<OrderRejected>(reuse), std::size_t{0});
    CHECK_EQ(countOf<OrderResting>(reuse), std::size_t{1});

    // Rejected commands still consume a command sequence number. The log
    // is the record of what was *submitted*, not only of what worked,
    // which is what lets a replica replay it verbatim.
    MatchingEngine seq(kInstrument);
    seq.apply(limit(1, Side::Buy, 100, 0));
    CHECK_EQ(seq.commandSequence(), Sequence{1});
    seq.apply(cancel(99));
    CHECK_EQ(seq.commandSequence(), Sequence{2});
    CHECK(seq.book().empty());

    // Self-crossing is permitted: this model has no participant
    // identity, so there is nobody to protect from trading with
    // themselves. Worth stating explicitly rather than leaving implied.
    MatchingEngine self(kInstrument);
    self.apply(limit(1, Side::Buy, 100, 10));
    EventList sc = self.apply(limit(2, Side::Sell, 100, 10));
    CHECK_EQ(countOf<Trade>(sc), std::size_t{1});

    // Large quantities do not overflow or wrap.
    MatchingEngine big(kInstrument);
    const Quantity huge = std::numeric_limits<Quantity>::max() / 4;
    big.apply(limit(1, Side::Sell, 100, huge));
    EventList b = big.apply(limit(2, Side::Buy, 100, huge));
    CHECK_EQ(nth<Trade>(b, 0).quantity, huge);
    CHECK(big.book().empty());

    // A crossed book cannot form: a bid that would sit above the best
    // ask trades instead of resting there.
    MatchingEngine cross(kInstrument);
    cross.apply(limit(1, Side::Sell, 100, 10));
    cross.apply(limit(2, Side::Buy, 105, 4));
    CHECK_EQ(cross.book().bestAsk().value(), Price{100});
    CHECK(!cross.book().bestBid().has_value());
}

}  // namespace

TEST_MAIN("test_edge_cases")
