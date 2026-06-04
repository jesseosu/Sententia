// Sententia - the command half of the command/event model.
//
// A Command is an *intent* submitted to the engine from outside. It is
// the unit that later phases will replicate: if two nodes apply the same
// ordered sequence of commands to the same starting state, they must end
// up in the same state and emit the same events.
//
// Commands therefore carry no engine-assigned data. No timestamps, no
// sequence numbers, no derived fields. Anything the engine decides is an
// output, not an input.
#pragma once

#include <variant>

#include "sententia/types.hpp"

namespace sententia {

struct NewOrder {
    OrderId id{};
    InstrumentId instrument{};
    Side side{Side::Buy};
    OrderType type{OrderType::Limit};
    TimeInForce tif{TimeInForce::GoodTillCancel};
    // Ignored for market orders. Callers should set it to 0.
    Price price{};
    Quantity quantity{};

    friend bool operator==(const NewOrder&, const NewOrder&) = default;
};

struct CancelOrder {
    OrderId id{};

    friend bool operator==(const CancelOrder&, const CancelOrder&) = default;
};

using Command = std::variant<NewOrder, CancelOrder>;

}  // namespace sententia
