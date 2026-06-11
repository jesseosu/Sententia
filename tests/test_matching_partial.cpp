// Partial fills in both directions: aggressor bigger than the book, and
// book bigger than the aggressor.
#include "sententia/engine.hpp"

#include "harness.hpp"
#include "support.hpp"

using namespace sententia;
using namespace support;

namespace {

void run() {
    // Aggressor larger than available liquidity: fills what it can and
    // rests the residual at its own limit.
    MatchingEngine engine(kInstrument);
    engine.apply(limit(1, Side::Sell, 100, 4));
    EventList out = engine.apply(limit(2, Side::Buy, 100, 10));

    CHECK_EQ(countOf<Trade>(out), std::size_t{1});
    CHECK_EQ(nth<Trade>(out, 0).quantity, Quantity{4});
    CHECK_EQ(countOf<OrderResting>(out), std::size_t{1});
    CHECK_EQ(nth<OrderResting>(out, 0).quantity, Quantity{6});
    CHECK_EQ(engine.book().bestBid().value(), Price{100});
    CHECK(!engine.book().bestAsk().has_value());
    CHECK_EQ(engine.book().quantityAt(Side::Buy, 100), Quantity{6});

    // Aggressor smaller than the resting order: the resting order stays
    // with reduced quantity and keeps its place in the queue.
    MatchingEngine small(kInstrument);
    small.apply(limit(1, Side::Sell, 100, 10));
    small.apply(limit(2, Side::Sell, 100, 10));
    EventList out2 = small.apply(limit(3, Side::Buy, 100, 3));

    CHECK_EQ(countOf<Trade>(out2), std::size_t{1});
    CHECK_EQ(nth<Trade>(out2, 0).restingId, OrderId{1});
    CHECK_EQ(nth<Trade>(out2, 0).quantity, Quantity{3});
    CHECK_EQ(countOf<OrderResting>(out2), std::size_t{0});
    CHECK_EQ(small.book().quantityAt(Side::Sell, 100), Quantity{17});

    // The partially filled order is still the front of the queue, so the
    // next aggressor hits it again before touching order 2.
    EventList out3 = small.apply(limit(4, Side::Buy, 100, 7));
    CHECK_EQ(countOf<Trade>(out3), std::size_t{1});
    CHECK_EQ(nth<Trade>(out3, 0).restingId, OrderId{1});
    CHECK_EQ(nth<Trade>(out3, 0).quantity, Quantity{7});
    CHECK(!small.book().contains(1));

    // Sweeping several price levels produces one trade per resting
    // order, in price then time order.
    MatchingEngine sweep(kInstrument);
    sweep.apply(limit(1, Side::Sell, 100, 5));
    sweep.apply(limit(2, Side::Sell, 101, 5));
    sweep.apply(limit(3, Side::Sell, 102, 5));
    EventList out4 = sweep.apply(limit(4, Side::Buy, 101, 12));

    CHECK_EQ(countOf<Trade>(out4), std::size_t{2});
    CHECK_EQ(nth<Trade>(out4, 0).price, Price{100});
    CHECK_EQ(nth<Trade>(out4, 1).price, Price{101});
    // The limit stops the sweep at 101; 2 units rest at 101 as a bid.
    CHECK_EQ(countOf<OrderResting>(out4), std::size_t{1});
    CHECK_EQ(nth<OrderResting>(out4, 0).quantity, Quantity{2});
    CHECK_EQ(sweep.book().bestAsk().value(), Price{102});
    CHECK_EQ(sweep.book().bestBid().value(), Price{101});
}

}  // namespace

TEST_MAIN("test_matching_partial")
