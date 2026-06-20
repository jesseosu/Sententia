// Property-based testing of book invariants.
//
// Rather than asserting specific outputs for specific inputs, this
// drives thousands of generated command sequences and, after every
// single command, asserts properties that must hold for any book in any
// state. It finds the cases nobody thought to write a unit test for.
//
// The generator is seeded and reproducible, so a failure here is a
// failure you can replay exactly. That is the same property the engine
// itself provides, applied to its own test suite.
#include "sententia/engine.hpp"

#include <cstddef>
#include <unordered_map>
#include <vector>

#include "harness.hpp"
#include "support.hpp"

using namespace sententia;
using namespace support;

namespace {

struct Invariants {
    std::size_t violations = 0;

    void check(const MatchingEngine& engine) {
        const OrderBook& book = engine.book();

        std::size_t counted = 0;

        // Bids are strictly descending, asks strictly ascending, and no
        // price level is ever empty or holds a zero-quantity order.
        Price previousBid = 0;
        bool firstBid = true;
        for (const auto& [price, queue] : book.bids()) {
            if (!firstBid && price >= previousBid) {
                ++violations;
            }
            firstBid = false;
            previousBid = price;
            if (queue.empty() || price <= 0) {
                ++violations;
            }
            Sequence previousArrival = 0;
            for (const RestingOrder& o : queue) {
                if (o.quantity == 0 || o.side != Side::Buy || o.price != price) {
                    ++violations;
                }
                // Time priority: arrival ordinals ascend within a level.
                if (o.arrival <= previousArrival) {
                    ++violations;
                }
                previousArrival = o.arrival;
                ++counted;
            }
        }

        Price previousAsk = 0;
        bool firstAsk = true;
        for (const auto& [price, queue] : book.asks()) {
            if (!firstAsk && price <= previousAsk) {
                ++violations;
            }
            firstAsk = false;
            previousAsk = price;
            if (queue.empty() || price <= 0) {
                ++violations;
            }
            Sequence previousArrival = 0;
            for (const RestingOrder& o : queue) {
                if (o.quantity == 0 || o.side != Side::Sell || o.price != price) {
                    ++violations;
                }
                if (o.arrival <= previousArrival) {
                    ++violations;
                }
                previousArrival = o.arrival;
                ++counted;
            }
        }

        // The id index and the levels agree about how many orders exist.
        if (counted != book.orderCount()) {
            ++violations;
        }

        // The book is never crossed. Any bid at or above the best ask
        // should have traded instead of resting.
        if (book.bestBid().has_value() && book.bestAsk().has_value()) {
            if (book.bestBid().value() >= book.bestAsk().value()) {
                ++violations;
            }
        }
    }
};

// Every unit of quantity is accounted for: what was accepted equals what
// traded, plus what rests, plus what was cancelled. Checked over a whole
// run rather than per command.
void checkConservation(const EventList& events, const MatchingEngine& engine) {
    Quantity accepted = 0;
    Quantity traded = 0;
    Quantity cancelled = 0;

    for (const Event& e : events) {
        if (const auto* a = std::get_if<OrderAccepted>(&e)) {
            accepted += a->quantity;
        } else if (const auto* t = std::get_if<Trade>(&e)) {
            // Each trade consumes one unit from the aggressor and one
            // from the resting order, so it counts twice against the
            // accepted total.
            traded += t->quantity * 2;
        } else if (const auto* c = std::get_if<OrderCancelled>(&e)) {
            cancelled += c->remaining;
        }
    }

    Quantity resting = 0;
    for (const auto& [price, queue] : engine.book().bids()) {
        (void)price;
        for (const RestingOrder& o : queue) {
            resting += o.quantity;
        }
    }
    for (const auto& [price, queue] : engine.book().asks()) {
        (void)price;
        for (const RestingOrder& o : queue) {
            resting += o.quantity;
        }
    }

    CHECK_EQ(accepted, traded + cancelled + resting);
}

// Trades never execute outside the aggressor's limit.
void checkTradePrices(const std::vector<Command>& cmds, const EventList& events) {
    std::unordered_map<OrderId, NewOrder> submitted;
    for (const Command& c : cmds) {
        if (const auto* n = std::get_if<NewOrder>(&c)) {
            submitted[n->id] = *n;
        }
    }

    std::size_t violations = 0;
    for (const Event& e : events) {
        const auto* t = std::get_if<Trade>(&e);
        if (t == nullptr) {
            continue;
        }
        const auto it = submitted.find(t->aggressorId);
        if (it == submitted.end()) {
            continue;
        }
        const NewOrder& order = it->second;
        if (order.type == OrderType::Market) {
            continue;
        }
        const bool ok = order.side == Side::Buy ? t->price <= order.price : t->price >= order.price;
        if (!ok || t->quantity == 0) {
            ++violations;
        }
    }
    CHECK_EQ(violations, std::size_t{0});
}

void run() {
    // Many independent seeds, so a bug that only shows up in one shape
    // of order flow still gets found.
    for (std::uint64_t seed = 1; seed <= 40; ++seed) {
        const std::vector<Command> cmds = generateCommands(seed * 7919ULL, 1500);

        MatchingEngine engine(kInstrument);
        EventList events;
        Invariants inv;
        Sequence lastEventSeq = 0;

        for (const Command& c : cmds) {
            const std::size_t before = events.size();
            engine.apply(c, events);
            // Every command produces at least one event: silence is
            // never a valid response, because a replica needs to see
            // that the command was processed.
            CHECK(events.size() > before);
            // Event sequence numbers are gapless and strictly ascending.
            for (std::size_t i = before; i < events.size(); ++i) {
                CHECK_EQ(header(events[i]).eventSeq, lastEventSeq + 1);
                lastEventSeq = header(events[i]).eventSeq;
            }
            inv.check(engine);
        }

        CHECK_EQ(inv.violations, std::size_t{0});
        checkConservation(events, engine);
        checkTradePrices(cmds, events);
    }
}

}  // namespace

TEST_MAIN("test_invariants")
