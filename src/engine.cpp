#include "sententia/engine.hpp"

#include <algorithm>
#include <string>
#include <type_traits>
#include <variant>

namespace sententia {
namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

inline void mix(std::uint64_t& h, std::uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) {
        h ^= (v >> (i * 8)) & 0xFFULL;
        h *= kFnvPrime;
    }
}

// Would this incoming order cross the given resting price?
inline bool crosses(Side incomingSide, OrderType type, Price limit, Price resting) noexcept {
    if (type == OrderType::Market) {
        return true;
    }
    return incomingSide == Side::Buy ? resting <= limit : resting >= limit;
}

}  // namespace

MatchingEngine::MatchingEngine(InstrumentId instrument) noexcept : instrument_(instrument) {}

EventHeader MatchingEngine::nextHeader() noexcept {
    return EventHeader{commandSeq_, ++eventSeq_};
}

void MatchingEngine::emitTopOfBookIfChanged(EventList& out) {
    const TopOfBook now = book_.top();
    if (now == lastTop_) {
        return;
    }
    lastTop_ = now;
    TopOfBookChanged e;
    e.header = nextHeader();
    e.hasBid = now.hasBid;
    e.bidPrice = now.bidPrice;
    e.bidQuantity = now.bidQuantity;
    e.hasAsk = now.hasAsk;
    e.askPrice = now.askPrice;
    e.askQuantity = now.askQuantity;
    out.push_back(e);
}

EventList MatchingEngine::apply(const Command& cmd) {
    EventList out;
    apply(cmd, out);
    return out;
}

void MatchingEngine::apply(const Command& cmd, EventList& out) {
    ++commandSeq_;
    std::visit(
        [&](const auto& c) {
            using T = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<T, NewOrder>) {
                applyNewOrder(c, out);
            } else {
                applyCancel(c, out);
            }
        },
        cmd);
}

void MatchingEngine::applyNewOrder(const NewOrder& cmd, EventList& out) {
    // Validation. Order matters: it is part of the observable contract,
    // so a command that violates several rules always reports the same
    // one. Changing this order changes the event stream.
    RejectReason reason = RejectReason::None;
    if (cmd.instrument != instrument_) {
        reason = RejectReason::WrongInstrument;
    } else if (cmd.quantity == 0) {
        reason = RejectReason::ZeroQuantity;
    } else if (cmd.type == OrderType::Limit && cmd.price <= 0) {
        reason = RejectReason::NonPositivePrice;
    } else if (cmd.type == OrderType::Market && cmd.tif != TimeInForce::ImmediateOrCancel) {
        // A market order cannot rest, so demanding GTC is incoherent.
        // Rejecting is better than silently reinterpreting the intent.
        reason = RejectReason::MarketOrderMustBeIoc;
    } else if (book_.contains(cmd.id)) {
        // Ids are unique among *live* orders. A filled or cancelled id
        // may be reused; the book is the only authority on liveness.
        reason = RejectReason::DuplicateOrderId;
    }

    if (reason != RejectReason::None) {
        OrderRejected e;
        e.header = nextHeader();
        e.id = cmd.id;
        e.reason = reason;
        out.push_back(e);
        return;
    }

    {
        OrderAccepted e;
        e.header = nextHeader();
        e.id = cmd.id;
        e.side = cmd.side;
        e.type = cmd.type;
        e.tif = cmd.tif;
        e.price = cmd.price;
        e.quantity = cmd.quantity;
        out.push_back(e);
    }

    const Quantity residual = match(cmd, cmd.quantity, out);

    if (residual > 0) {
        const bool canRest = cmd.type == OrderType::Limit && cmd.tif == TimeInForce::GoodTillCancel;
        if (canRest) {
            book_.rest(RestingOrder{cmd.id, cmd.side, cmd.price, residual, ++arrivalCounter_});
            OrderResting e;
            e.header = nextHeader();
            e.id = cmd.id;
            e.side = cmd.side;
            e.price = cmd.price;
            e.quantity = residual;
            out.push_back(e);
        } else {
            // IOC residual, or a market order that exhausted the book.
            OrderCancelled e;
            e.header = nextHeader();
            e.id = cmd.id;
            e.side = cmd.side;
            e.price = cmd.price;
            e.remaining = residual;
            e.engineInitiated = true;
            out.push_back(e);
        }
    }

    emitTopOfBookIfChanged(out);
}

Quantity MatchingEngine::match(const NewOrder& cmd, Quantity quantity, EventList& out) {
    const Side book = opposite(cmd.side);

    while (quantity > 0) {
        OrderQueue* level = book_.frontLevel(book);
        if (level == nullptr || level->empty()) {
            break;
        }
        const Price restingPrice = level->front().price;
        if (!crosses(cmd.side, cmd.type, cmd.price, restingPrice)) {
            break;
        }

        // Time priority: strictly the front of the level. The list is
        // maintained in arrival order, so this needs no comparison.
        const RestingOrder& front = level->front();
        const Quantity fill = std::min(quantity, front.quantity);
        const OrderId restingId = front.id;

        Trade t;
        t.header = nextHeader();
        t.aggressorId = cmd.id;
        t.restingId = restingId;
        t.aggressorSide = cmd.side;
        // Execution at the resting price. The resting order set the
        // terms and the aggressor accepted them, so price improvement
        // accrues to the aggressor.
        t.price = restingPrice;
        t.quantity = fill;
        out.push_back(t);

        quantity -= fill;
        // reduce() removes the order, and the level, when it empties.
        book_.reduce(restingId, fill);
    }

    return quantity;
}

void MatchingEngine::applyCancel(const CancelOrder& cmd, EventList& out) {
    auto removed = book_.cancel(cmd.id);
    if (!removed.has_value()) {
        CancelRejected e;
        e.header = nextHeader();
        e.id = cmd.id;
        e.reason = RejectReason::UnknownOrderId;
        out.push_back(e);
        return;
    }

    OrderCancelled e;
    e.header = nextHeader();
    e.id = removed->id;
    e.side = removed->side;
    e.price = removed->price;
    e.remaining = removed->quantity;
    e.engineInitiated = false;
    out.push_back(e);

    emitTopOfBookIfChanged(out);
}

std::uint64_t MatchingEngine::stateChecksum() const noexcept {
    std::uint64_t h = book_.checksum();
    mix(h, commandSeq_);
    mix(h, eventSeq_);
    mix(h, arrivalCounter_);
    mix(h, static_cast<std::uint64_t>(instrument_));
    return h;
}

std::uint64_t hashEvents(const EventList& events) {
    std::uint64_t h = kFnvOffset;
    for (const Event& e : events) {
        mix(h, static_cast<std::uint64_t>(e.index()));
        const std::string text = toString(e);
        for (const char c : text) {
            h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
            h *= kFnvPrime;
        }
    }
    return h;
}

}  // namespace sententia
