// The core fairness property: price first, then arrival order, and
// nothing else. In particular not quantity, and not wall-clock time.
#include "sententia/engine.hpp"

#include "harness.hpp"
#include "support.hpp"

using namespace sententia;
using namespace support;

namespace {

void run() {
    // Time priority: two identical bids, the older one fills first.
    MatchingEngine engine(kInstrument);
    engine.apply(limit(1, Side::Buy, 100, 5));
    engine.apply(limit(2, Side::Buy, 100, 5));
    EventList out = engine.apply(limit(3, Side::Sell, 100, 5));

    CHECK_EQ(countOf<Trade>(out), std::size_t{1});
    CHECK_EQ(nth<Trade>(out, 0).restingId, OrderId{1});
    CHECK(!engine.book().contains(1));
    CHECK(engine.book().contains(2));

    // Quantity does not confer priority. A large late order still queues
    // behind a small early one.
    MatchingEngine sized(kInstrument);
    sized.apply(limit(1, Side::Buy, 100, 1));
    sized.apply(limit(2, Side::Buy, 100, 1000));
    EventList out2 = sized.apply(limit(3, Side::Sell, 100, 1));
    CHECK_EQ(nth<Trade>(out2, 0).restingId, OrderId{1});

    // Price priority beats time priority: a later, better-priced bid
    // fills before an earlier, worse-priced one.
    MatchingEngine priced(kInstrument);
    priced.apply(limit(1, Side::Buy, 100, 5));
    priced.apply(limit(2, Side::Buy, 101, 5));
    EventList out3 = priced.apply(limit(3, Side::Sell, 100, 10));
    CHECK_EQ(countOf<Trade>(out3), std::size_t{2});
    CHECK_EQ(nth<Trade>(out3, 0).restingId, OrderId{2});
    CHECK_EQ(nth<Trade>(out3, 0).price, Price{101});
    CHECK_EQ(nth<Trade>(out3, 1).restingId, OrderId{1});
    CHECK_EQ(nth<Trade>(out3, 1).price, Price{100});

    // Same rules on the ask side, mirrored.
    MatchingEngine asks(kInstrument);
    asks.apply(limit(1, Side::Sell, 101, 5));
    asks.apply(limit(2, Side::Sell, 100, 5));
    asks.apply(limit(3, Side::Sell, 100, 5));
    EventList out4 = asks.apply(limit(4, Side::Buy, 101, 15));
    CHECK_EQ(countOf<Trade>(out4), std::size_t{3});
    CHECK_EQ(nth<Trade>(out4, 0).restingId, OrderId{2});
    CHECK_EQ(nth<Trade>(out4, 1).restingId, OrderId{3});
    CHECK_EQ(nth<Trade>(out4, 2).restingId, OrderId{1});

    // Cancelling and re-entering loses priority. This is the property
    // that makes arrival ordinals, not order ids, the tie-break.
    MatchingEngine requeue(kInstrument);
    requeue.apply(limit(1, Side::Buy, 100, 5));
    requeue.apply(limit(2, Side::Buy, 100, 5));
    requeue.apply(cancel(1));
    requeue.apply(limit(1, Side::Buy, 100, 5));
    EventList out5 = requeue.apply(limit(3, Side::Sell, 100, 5));
    CHECK_EQ(nth<Trade>(out5, 0).restingId, OrderId{2});
}

}  // namespace

TEST_MAIN("test_price_time_priority")
