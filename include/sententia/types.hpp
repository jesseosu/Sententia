// Sententia - core value types for the matching engine.
//
// Everything here is a plain value type with no behaviour beyond
// comparison. The engine core is built from these so that its entire
// state is trivially copyable, hashable, and reproducible.
#pragma once

#include <cstdint>

namespace sententia {

using OrderId = std::uint64_t;
using InstrumentId = std::uint32_t;

// Prices are integer ticks, never floating point. Floating point
// arithmetic is deterministic on a fixed platform but not portably so
// across compilers and optimisation levels, and replication in later
// phases requires bit-identical results across machines.
using Price = std::int64_t;

using Quantity = std::uint64_t;

// Logical time. The engine assigns one sequence number per command it
// accepts and one per event it emits. This is the core's *only* notion
// of time: there is no wall clock anywhere inside it.
using Sequence = std::uint64_t;

enum class Side : std::uint8_t {
    Buy = 0,
    Sell = 1,
};

enum class OrderType : std::uint8_t {
    Limit = 0,
    Market = 1,
};

enum class TimeInForce : std::uint8_t {
    // Rest on the book until filled or cancelled.
    GoodTillCancel = 0,
    // Match what is available now, cancel the remainder.
    ImmediateOrCancel = 1,
};

enum class RejectReason : std::uint8_t {
    None = 0,
    DuplicateOrderId = 1,
    UnknownOrderId = 2,
    ZeroQuantity = 3,
    NonPositivePrice = 4,
    WrongInstrument = 5,
    MarketOrderMustBeIoc = 6,
};

constexpr Side opposite(Side s) noexcept {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

const char* toString(Side s) noexcept;
const char* toString(OrderType t) noexcept;
const char* toString(TimeInForce t) noexcept;
const char* toString(RejectReason r) noexcept;

}  // namespace sententia
