// Order book mechanics in isolation: resting, cancelling, reducing,
// level bookkeeping, and top of book.
#include "sententia/order_book.hpp"

#include "harness.hpp"

using namespace sententia;

namespace {

RestingOrder o(OrderId id, Side side, Price px, Quantity qty, Sequence arrival) {
    return RestingOrder{id, side, px, qty, arrival};
}

void run() {
    OrderBook book;
    CHECK(book.empty());
    CHECK(!book.bestBid().has_value());
    CHECK(!book.bestAsk().has_value());

    book.rest(o(1, Side::Buy, 100, 10, 1));
    book.rest(o(2, Side::Buy, 101, 5, 2));
    book.rest(o(3, Side::Sell, 105, 7, 3));
    book.rest(o(4, Side::Sell, 104, 3, 4));

    // Best bid is the highest buy price, best ask the lowest sell price.
    CHECK_EQ(book.bestBid().value(), Price{101});
    CHECK_EQ(book.bestAsk().value(), Price{104});
    CHECK_EQ(book.orderCount(), std::size_t{4});

    const TopOfBook top = book.top();
    CHECK(top.hasBid && top.hasAsk);
    CHECK_EQ(top.bidPrice, Price{101});
    CHECK_EQ(top.bidQuantity, Quantity{5});
    CHECK_EQ(top.askPrice, Price{104});
    CHECK_EQ(top.askQuantity, Quantity{3});

    // Two orders at one price aggregate into that level's quantity.
    book.rest(o(5, Side::Buy, 101, 8, 5));
    CHECK_EQ(book.quantityAt(Side::Buy, 101), Quantity{13});
    CHECK_EQ(book.top().bidQuantity, Quantity{13});

    // Cancelling the middle of a level leaves the rest intact.
    const auto removed = book.cancel(2);
    CHECK(removed.has_value());
    CHECK_EQ(removed->id, OrderId{2});
    CHECK_EQ(removed->quantity, Quantity{5});
    CHECK_EQ(book.quantityAt(Side::Buy, 101), Quantity{8});
    CHECK(!book.contains(2));

    // Cancelling something absent is reported, not asserted.
    CHECK(!book.cancel(999).has_value());

    // A partial reduce keeps the order; a full reduce removes it and,
    // when the level empties, the level too.
    book.reduce(5, 3);
    CHECK_EQ(book.quantityAt(Side::Buy, 101), Quantity{5});
    book.reduce(5, 5);
    CHECK(!book.contains(5));
    CHECK_EQ(book.bestBid().value(), Price{100});

    // Checksums track state, not history: two books built by different
    // routes to the same state agree.
    OrderBook other;
    other.rest(o(1, Side::Buy, 100, 10, 1));
    other.rest(o(3, Side::Sell, 105, 7, 3));
    other.rest(o(4, Side::Sell, 104, 3, 4));
    CHECK_EQ(book.checksum(), other.checksum());

    // And a different state gives a different checksum.
    other.reduce(1, 1);
    CHECK(book.checksum() != other.checksum());
}

}  // namespace

TEST_MAIN("test_order_book")
