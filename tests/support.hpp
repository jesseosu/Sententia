// Shared helpers for the Sententia test suite.
//
// The deterministic command generator lives here rather than in the
// library on purpose: randomness belongs at the edges. The core never
// sees an RNG; the *test* owns the seed and the sequence, which is what
// makes "same input, same output" a meaningful claim.
#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "sententia/command.hpp"
#include "sententia/engine.hpp"
#include "sententia/event.hpp"

namespace support {

using namespace sententia;

constexpr InstrumentId kInstrument = 42;

inline NewOrder limit(OrderId id, Side side, Price price, Quantity qty,
                      TimeInForce tif = TimeInForce::GoodTillCancel) {
    NewOrder o;
    o.id = id;
    o.instrument = kInstrument;
    o.side = side;
    o.type = OrderType::Limit;
    o.tif = tif;
    o.price = price;
    o.quantity = qty;
    return o;
}

inline NewOrder market(OrderId id, Side side, Quantity qty) {
    NewOrder o;
    o.id = id;
    o.instrument = kInstrument;
    o.side = side;
    o.type = OrderType::Market;
    o.tif = TimeInForce::ImmediateOrCancel;
    o.price = 0;
    o.quantity = qty;
    return o;
}

inline CancelOrder cancel(OrderId id) {
    return CancelOrder{id};
}

// Counts events of one alternative in a stream.
template <typename T>
inline std::size_t countOf(const EventList& events) {
    std::size_t n = 0;
    for (const Event& e : events) {
        if (std::holds_alternative<T>(e)) {
            ++n;
        }
    }
    return n;
}

// Returns the nth event of a given alternative, or throws.
template <typename T>
inline const T& nth(const EventList& events, std::size_t n) {
    std::size_t seen = 0;
    for (const Event& e : events) {
        if (const T* p = std::get_if<T>(&e)) {
            if (seen == n) {
                return *p;
            }
            ++seen;
        }
    }
    throw std::runtime_error("no such event");
}

inline std::vector<std::string> render(const EventList& events) {
    std::vector<std::string> lines;
    lines.reserve(events.size());
    for (const Event& e : events) {
        lines.push_back(toString(e));
    }
    return lines;
}

// A 64-bit linear congruential generator with fixed constants. Chosen
// over std::mt19937 plus std::uniform_int_distribution because the
// standard library's distributions are not specified to produce the same
// values across implementations, which would make a cross-platform
// determinism test compare two different inputs and call it a bug.
class Lcg {
public:
    explicit Lcg(std::uint64_t seed) noexcept : state_(seed) {}

    std::uint64_t next() noexcept {
        state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return state_ >> 17;
    }

    std::uint64_t inRange(std::uint64_t lo, std::uint64_t hi) noexcept {
        return lo + next() % (hi - lo + 1);
    }

private:
    std::uint64_t state_;
};

// Builds a fixed pseudo-random but fully reproducible command stream:
// a mix of resting limits, crossing limits, market orders and cancels.
inline std::vector<Command> generateCommands(std::uint64_t seed, std::size_t count) {
    Lcg rng(seed);
    std::vector<Command> cmds;
    cmds.reserve(count);
    std::vector<OrderId> live;
    OrderId nextId = 1;

    for (std::size_t i = 0; i < count; ++i) {
        const std::uint64_t roll = rng.inRange(0, 99);
        if (roll < 15 && !live.empty()) {
            const auto idx = static_cast<std::size_t>(rng.inRange(0, live.size() - 1));
            cmds.push_back(cancel(live[idx]));
            live.erase(live.begin() + static_cast<std::ptrdiff_t>(idx));
            continue;
        }

        const Side side = (rng.inRange(0, 1) == 0) ? Side::Buy : Side::Sell;
        const Quantity qty = rng.inRange(1, 50);
        const OrderId id = nextId++;

        if (roll >= 92) {
            cmds.push_back(market(id, side, qty));
            continue;
        }

        const Price price = static_cast<Price>(rng.inRange(9950, 10050));
        const TimeInForce tif =
            (roll >= 85) ? TimeInForce::ImmediateOrCancel : TimeInForce::GoodTillCancel;
        cmds.push_back(limit(id, side, price, qty, tif));
        if (tif == TimeInForce::GoodTillCancel) {
            live.push_back(id);
        }
    }
    return cmds;
}

// Applies a whole command stream to a fresh engine.
inline EventList runAll(MatchingEngine& engine, const std::vector<Command>& cmds) {
    EventList out;
    for (const Command& c : cmds) {
        engine.apply(c, out);
    }
    return out;
}

}  // namespace support
