// Market orders and immediate-or-cancel: the two order flavours that can
// never rest, and the engine-initiated cancels they produce.
#include "sententia/engine.hpp"

#include "harness.hpp"
#include "support.hpp"

using namespace sententia;
using namespace support;

namespace {

void run() {
    // A market order sweeps every level regardless of price.
    MatchingEngine engine(kInstrument);
    engine.apply(limit(1, Side::Sell, 100, 5));
    engine.apply(limit(2, Side::Sell, 200, 5));
    engine.apply(limit(3, Side::Sell, 300, 5));
    EventList out = engine.apply(market(4, Side::Buy, 12));

    CHECK_EQ(countOf<Trade>(out), std::size_t{3});
    CHECK_EQ(nth<Trade>(out, 0).price, Price{100});
    CHECK_EQ(nth<Trade>(out, 1).price, Price{200});
    CHECK_EQ(nth<Trade>(out, 2).price, Price{300});
    CHECK_EQ(nth<Trade>(out, 2).quantity, Quantity{2});
    // The aggressor was filled in full, so there is nothing to cancel
    // and 3 units are left resting at the top level it partly consumed.
    CHECK_EQ(countOf<OrderCancelled>(out), std::size_t{0});
    CHECK_EQ(engine.book().quantityAt(Side::Sell, 300), Quantity{3});

    // A market order into an empty book is cancelled in full.
    MatchingEngine dry(kInstrument);
    EventList out2 = dry.apply(market(1, Side::Buy, 10));
    CHECK_EQ(countOf<Trade>(out2), std::size_t{0});
    CHECK_EQ(countOf<OrderCancelled>(out2), std::size_t{1});
    CHECK(nth<OrderCancelled>(out2, 0).engineInitiated);
    CHECK_EQ(nth<OrderCancelled>(out2, 0).remaining, Quantity{10});
    CHECK(dry.book().empty());
    // Nothing entered the book, so the top of book did not change.
    CHECK_EQ(countOf<TopOfBookChanged>(out2), std::size_t{0});

    // A market order that outsizes the book fills what it can, then the
    // remainder is cancelled by the engine.
    MatchingEngine thin(kInstrument);
    thin.apply(limit(1, Side::Sell, 100, 4));
    EventList out3 = thin.apply(market(2, Side::Buy, 10));
    CHECK_EQ(countOf<Trade>(out3), std::size_t{1});
    CHECK_EQ(nth<OrderCancelled>(out3, 0).remaining, Quantity{6});
    CHECK(nth<OrderCancelled>(out3, 0).engineInitiated);

    // A market order demanding GTC is incoherent and gets rejected
    // rather than quietly reinterpreted.
    MatchingEngine bad(kInstrument);
    NewOrder gtcMarket = market(1, Side::Buy, 10);
    gtcMarket.tif = TimeInForce::GoodTillCancel;
    EventList out4 = bad.apply(gtcMarket);
    CHECK_EQ(countOf<OrderRejected>(out4), std::size_t{1});
    CHECK(nth<OrderRejected>(out4, 0).reason == RejectReason::MarketOrderMustBeIoc);

    // IOC takes what is available at its limit and cancels the rest; it
    // never rests.
    MatchingEngine ioc(kInstrument);
    ioc.apply(limit(1, Side::Sell, 100, 4));
    EventList out5 = ioc.apply(limit(2, Side::Buy, 100, 10, TimeInForce::ImmediateOrCancel));
    CHECK_EQ(countOf<Trade>(out5), std::size_t{1});
    CHECK_EQ(countOf<OrderResting>(out5), std::size_t{0});
    CHECK_EQ(countOf<OrderCancelled>(out5), std::size_t{1});
    CHECK(nth<OrderCancelled>(out5, 0).engineInitiated);
    CHECK(ioc.book().empty());

    // An IOC that crosses nothing is cancelled in full.
    MatchingEngine miss(kInstrument);
    miss.apply(limit(1, Side::Sell, 105, 4));
    EventList out6 = miss.apply(limit(2, Side::Buy, 100, 10, TimeInForce::ImmediateOrCancel));
    CHECK_EQ(countOf<Trade>(out6), std::size_t{0});
    CHECK_EQ(nth<OrderCancelled>(out6, 0).remaining, Quantity{10});
    CHECK_EQ(miss.book().orderCount(), std::size_t{1});
}

}  // namespace

TEST_MAIN("test_order_types")
