#include "sententia/order_book.hpp"

#include <cassert>

namespace sententia {
namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

inline void hashBytes(std::uint64_t& h, const void* data, std::size_t len) noexcept {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < len; ++i) {
        h ^= static_cast<std::uint64_t>(p[i]);
        h *= kFnvPrime;
    }
}

template <typename T>
inline void hashValue(std::uint64_t& h, const T& v) noexcept {
    hashBytes(h, &v, sizeof(T));
}

template <typename Levels>
inline void hashLevels(std::uint64_t& h, const Levels& levels) noexcept {
    for (const auto& [price, queue] : levels) {
        hashValue(h, price);
        const auto count = static_cast<std::uint64_t>(queue.size());
        hashValue(h, count);
        for (const RestingOrder& o : queue) {
            hashValue(h, o.id);
            hashValue(h, o.quantity);
            hashValue(h, o.arrival);
        }
    }
}

}  // namespace

void OrderBook::rest(const RestingOrder& order) {
    assert(order.quantity > 0 && "resting a zero-quantity order");
    assert(index_.find(order.id) == index_.end() && "duplicate order id in book");

    if (order.side == Side::Buy) {
        OrderQueue& q = bids_[order.price];
        q.push_back(order);
        index_.emplace(order.id, Locator{order.side, order.price, std::prev(q.end())});
    } else {
        OrderQueue& q = asks_[order.price];
        q.push_back(order);
        index_.emplace(order.id, Locator{order.side, order.price, std::prev(q.end())});
    }
}

void OrderBook::eraseLocated(const Locator& loc, OrderId id) {
    if (loc.side == Side::Buy) {
        auto level = bids_.find(loc.price);
        assert(level != bids_.end());
        level->second.erase(loc.it);
        if (level->second.empty()) {
            bids_.erase(level);
        }
    } else {
        auto level = asks_.find(loc.price);
        assert(level != asks_.end());
        level->second.erase(loc.it);
        if (level->second.empty()) {
            asks_.erase(level);
        }
    }
    index_.erase(id);
}

std::optional<RestingOrder> OrderBook::cancel(OrderId id) {
    auto it = index_.find(id);
    if (it == index_.end()) {
        return std::nullopt;
    }
    const Locator loc = it->second;
    const RestingOrder removed = *loc.it;
    eraseLocated(loc, id);
    return removed;
}

bool OrderBook::contains(OrderId id) const noexcept {
    return index_.find(id) != index_.end();
}

void OrderBook::reduce(OrderId id, Quantity by) {
    auto it = index_.find(id);
    if (it == index_.end()) {
        return;
    }
    const Locator loc = it->second;
    assert(loc.it->quantity >= by && "reducing a resting order below zero");
    loc.it->quantity -= by;
    if (loc.it->quantity == 0) {
        eraseLocated(loc, id);
    }
}

std::optional<Price> OrderBook::bestBid() const noexcept {
    if (bids_.empty()) {
        return std::nullopt;
    }
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::bestAsk() const noexcept {
    if (asks_.empty()) {
        return std::nullopt;
    }
    return asks_.begin()->first;
}

TopOfBook OrderBook::top() const noexcept {
    TopOfBook t;
    if (!bids_.empty()) {
        const auto& level = *bids_.begin();
        t.hasBid = true;
        t.bidPrice = level.first;
        for (const RestingOrder& o : level.second) {
            t.bidQuantity += o.quantity;
        }
    }
    if (!asks_.empty()) {
        const auto& level = *asks_.begin();
        t.hasAsk = true;
        t.askPrice = level.first;
        for (const RestingOrder& o : level.second) {
            t.askQuantity += o.quantity;
        }
    }
    return t;
}

Quantity OrderBook::quantityAt(Side side, Price price) const noexcept {
    Quantity total = 0;
    if (side == Side::Buy) {
        auto it = bids_.find(price);
        if (it == bids_.end()) {
            return 0;
        }
        for (const RestingOrder& o : it->second) {
            total += o.quantity;
        }
    } else {
        auto it = asks_.find(price);
        if (it == asks_.end()) {
            return 0;
        }
        for (const RestingOrder& o : it->second) {
            total += o.quantity;
        }
    }
    return total;
}

OrderQueue* OrderBook::frontLevel(Side side) noexcept {
    if (side == Side::Buy) {
        return bids_.empty() ? nullptr : &bids_.begin()->second;
    }
    return asks_.empty() ? nullptr : &asks_.begin()->second;
}

std::uint64_t OrderBook::checksum() const noexcept {
    std::uint64_t h = kFnvOffset;
    const std::uint64_t bidCount = static_cast<std::uint64_t>(bids_.size());
    hashValue(h, bidCount);
    hashLevels(h, bids_);
    const std::uint64_t askCount = static_cast<std::uint64_t>(asks_.size());
    hashValue(h, askCount);
    hashLevels(h, asks_);
    return h;
}

}  // namespace sententia
