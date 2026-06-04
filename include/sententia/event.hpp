// Sententia - the event half of the command/event model.
//
// An Event is a *fact*: something the engine has already decided. Events
// are the engine's only output. The book state is fully derivable by
// replaying the event stream, which is what makes the log the unit of
// replication in Phase 3 and the unit of recovery in Phase 5.
//
// Every event carries the sequence number of the command that produced
// it, plus its own ordinal within the run, so the stream is totally
// ordered and comparable across runs.
#pragma once

#include <string>
#include <variant>
#include <vector>

#include "sententia/types.hpp"

namespace sententia {

// Common header on every event.
struct EventHeader {
    // Sequence number of the command that caused this event.
    Sequence commandSeq{};
    // Monotonic ordinal of this event within the engine's lifetime.
    Sequence eventSeq{};

    friend bool operator==(const EventHeader&, const EventHeader&) = default;
};

struct OrderAccepted {
    EventHeader header{};
    OrderId id{};
    Side side{Side::Buy};
    OrderType type{OrderType::Limit};
    TimeInForce tif{TimeInForce::GoodTillCancel};
    Price price{};
    Quantity quantity{};

    friend bool operator==(const OrderAccepted&, const OrderAccepted&) = default;
};

struct OrderRejected {
    EventHeader header{};
    OrderId id{};
    RejectReason reason{RejectReason::None};

    friend bool operator==(const OrderRejected&, const OrderRejected&) = default;
};

struct Trade {
    EventHeader header{};
    OrderId aggressorId{};
    OrderId restingId{};
    // Side of the incoming (aggressing) order.
    Side aggressorSide{Side::Buy};
    // Trades always execute at the resting order's price. The resting
    // order set the terms; the aggressor accepted them.
    Price price{};
    Quantity quantity{};

    friend bool operator==(const Trade&, const Trade&) = default;
};

// Emitted when an order comes to rest on the book with residual quantity.
struct OrderResting {
    EventHeader header{};
    OrderId id{};
    Side side{Side::Buy};
    Price price{};
    Quantity quantity{};

    friend bool operator==(const OrderResting&, const OrderResting&) = default;
};

struct OrderCancelled {
    EventHeader header{};
    OrderId id{};
    Side side{Side::Buy};
    Price price{};
    // Quantity removed from the book by the cancel.
    Quantity remaining{};
    // True when the engine cancelled it (IOC residual, market order with
    // no liquidity) rather than a client asking.
    bool engineInitiated{false};

    friend bool operator==(const OrderCancelled&, const OrderCancelled&) = default;
};

struct CancelRejected {
    EventHeader header{};
    OrderId id{};
    RejectReason reason{RejectReason::None};

    friend bool operator==(const CancelRejected&, const CancelRejected&) = default;
};

// Emitted whenever the best bid or best ask changes, at most once per
// command. Downstream market data publication in later phases consumes
// this; here it is simply another deterministic output.
struct TopOfBookChanged {
    EventHeader header{};
    bool hasBid{false};
    Price bidPrice{};
    Quantity bidQuantity{};
    bool hasAsk{false};
    Price askPrice{};
    Quantity askQuantity{};

    friend bool operator==(const TopOfBookChanged&, const TopOfBookChanged&) = default;
};

using Event = std::variant<OrderAccepted, OrderRejected, Trade, OrderResting, OrderCancelled,
                           CancelRejected, TopOfBookChanged>;

const EventHeader& header(const Event& e) noexcept;

// Stable, human-readable, one-line rendering. Used by the replay CLI and
// by the determinism test, so the format is part of the contract: two
// runs that produce the same events produce byte-identical text.
std::string toString(const Event& e);

using EventList = std::vector<Event>;

}  // namespace sententia
