// Cancel flows: accepted cancels, rejected cancels, and the interaction
// between cancels and fills.
#include "sententia/engine.hpp"

#include "harness.hpp"
#include "support.hpp"

using namespace sententia;
using namespace support;

namespace {

void run() {
    MatchingEngine engine(kInstrument);
    engine.apply(limit(1, Side::Buy, 100, 10));

    EventList out = engine.apply(cancel(1));
    CHECK_EQ(countOf<OrderCancelled>(out), std::size_t{1});
    const OrderCancelled& c = nth<OrderCancelled>(out, 0);
    CHECK_EQ(c.id, OrderId{1});
    CHECK_EQ(c.remaining, Quantity{10});
    CHECK_EQ(c.price, Price{100});
    CHECK(c.side == Side::Buy);
    CHECK(!c.engineInitiated);
    // Removing the only bid empties the top of book, which is a change.
    CHECK_EQ(countOf<TopOfBookChanged>(out), std::size_t{1});
    CHECK(!nth<TopOfBookChanged>(out, 0).hasBid);
    CHECK(engine.book().empty());

    // Cancelling an unknown id is rejected, not silently ignored.
    EventList out2 = engine.apply(cancel(1));
    CHECK_EQ(countOf<CancelRejected>(out2), std::size_t{1});
    CHECK(nth<CancelRejected>(out2, 0).reason == RejectReason::UnknownOrderId);
    CHECK_EQ(countOf<TopOfBookChanged>(out2), std::size_t{0});

    EventList out3 = engine.apply(cancel(12345));
    CHECK_EQ(countOf<CancelRejected>(out3), std::size_t{1});

    // A partially filled order cancels with its residual quantity.
    MatchingEngine partial(kInstrument);
    partial.apply(limit(1, Side::Buy, 100, 10));
    partial.apply(limit(2, Side::Sell, 100, 4));
    EventList out4 = partial.apply(cancel(1));
    CHECK_EQ(nth<OrderCancelled>(out4, 0).remaining, Quantity{6});
    CHECK(partial.book().empty());

    // A fully filled order is gone, so cancelling it is a reject.
    MatchingEngine filled(kInstrument);
    filled.apply(limit(1, Side::Buy, 100, 10));
    filled.apply(limit(2, Side::Sell, 100, 10));
    EventList out5 = filled.apply(cancel(1));
    CHECK_EQ(countOf<CancelRejected>(out5), std::size_t{1});

    // Cancelling one of two orders at a level leaves the level standing
    // and does not change the top of book quantity reporting rules.
    MatchingEngine level(kInstrument);
    level.apply(limit(1, Side::Buy, 100, 10));
    level.apply(limit(2, Side::Buy, 100, 10));
    EventList out6 = level.apply(cancel(1));
    CHECK_EQ(countOf<OrderCancelled>(out6), std::size_t{1});
    CHECK_EQ(countOf<TopOfBookChanged>(out6), std::size_t{1});
    CHECK_EQ(nth<TopOfBookChanged>(out6, 0).bidQuantity, Quantity{10});
    CHECK_EQ(level.book().quantityAt(Side::Buy, 100), Quantity{10});

    // Cancelling a non-top order does not move the top of book, so no
    // top of book event is emitted.
    MatchingEngine deep(kInstrument);
    deep.apply(limit(1, Side::Buy, 100, 10));
    deep.apply(limit(2, Side::Buy, 99, 10));
    EventList out7 = deep.apply(cancel(2));
    CHECK_EQ(countOf<OrderCancelled>(out7), std::size_t{1});
    CHECK_EQ(countOf<TopOfBookChanged>(out7), std::size_t{0});
}

}  // namespace

TEST_MAIN("test_cancel")
