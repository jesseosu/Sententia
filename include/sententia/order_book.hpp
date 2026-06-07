// Sententia - price-time priority limit order book.
//
// Determinism notes, because they drove every container choice here:
//
//   * Price levels live in std::map with an explicit comparator, so
//     iteration order is defined by price and nothing else. A hash map
//     would iterate in an order that depends on the allocator and on
//     insertion history, which is exactly the kind of hidden
//     non-determinism that makes replication impossible.
//
//   * Orders within a level live in std::list, giving FIFO order plus
//     stable iterators, so a cancel is O(1) once the order is located
//     and does not disturb the relative order of anything else.
//
//   * The id index is an unordered_map. That is safe *only* because it
//     is never iterated: it serves point lookups exclusively. Iterating
//     it would reintroduce non-determinism. Enforced by convention and
//     documented in docs/determinism.md.
#pragma once

#include <cstddef>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>

#include "sententia/types.hpp"

namespace sententia {

struct RestingOrder {
    OrderId id{};
    Side side{Side::Buy};
    Price price{};
    Quantity quantity{};
    // Engine-assigned arrival ordinal. Ties within a price level are
    // broken by this, never by wall-clock time.
    Sequence arrival{};
};

// FIFO queue of orders at a single price.
using OrderQueue = std::list<RestingOrder>;

struct TopOfBook {
    bool hasBid{false};
    Price bidPrice{};
    Quantity bidQuantity{};
    bool hasAsk{false};
    Price askPrice{};
    Quantity askQuantity{};

    friend bool operator==(const TopOfBook&, const TopOfBook&) = default;
};

class OrderBook {
public:
    // Best bid first means descending price; best ask first means ascending.
    using BidLevels = std::map<Price, OrderQueue, std::greater<Price>>;
    using AskLevels = std::map<Price, OrderQueue, std::less<Price>>;

    OrderBook() = default;

    // Places an order at the back of its price level. The caller is
    // responsible for having matched it first; this does no crossing.
    void rest(const RestingOrder& order);

    // Removes an order by id. Returns the removed order if present.
    std::optional<RestingOrder> cancel(OrderId id);

    bool contains(OrderId id) const noexcept;

    // Reduces the quantity of a resting order, removing it when it
    // reaches zero. Used by the matcher as fills consume resting size.
    void reduce(OrderId id, Quantity by);

    std::optional<Price> bestBid() const noexcept;
    std::optional<Price> bestAsk() const noexcept;
    TopOfBook top() const noexcept;

    Quantity quantityAt(Side side, Price price) const noexcept;
    std::size_t orderCount() const noexcept { return index_.size(); }
    bool empty() const noexcept { return index_.empty(); }

    const BidLevels& bids() const noexcept { return bids_; }
    const AskLevels& asks() const noexcept { return asks_; }

    // Mutable access to the front of the opposite book, for the matcher.
    OrderQueue* frontLevel(Side side) noexcept;

    // FNV-1a over the full book state walked in canonical order:
    // bids by descending price, asks by ascending price, orders in FIFO
    // order within each level. Two books with the same checksum have the
    // same observable state. This is the value replicas compare in the
    // later phases, and the value the determinism test pins.
    std::uint64_t checksum() const noexcept;

private:
    struct Locator {
        Side side{};
        Price price{};
        OrderQueue::iterator it{};
    };

    BidLevels bids_;
    AskLevels asks_;
    std::unordered_map<OrderId, Locator> index_;

    void eraseLocated(const Locator& loc, OrderId id);
};

}  // namespace sententia
