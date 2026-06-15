// Sententia single-node baseline benchmark.
//
// Establishes the number the distributed engine gets compared against in
// Phase 6. It measures the cost of MatchingEngine::apply and nothing
// else: no parsing, no printing, no allocation inside the timed region
// beyond what the engine itself does.
//
// The clock lives here, at the edge, and never inside the engine. That
// is the whole architectural point of Phase 1: timing is an observation
// made from outside a core that has no notion of real time.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "sententia/engine.hpp"

using namespace sententia;

namespace {

constexpr InstrumentId kInstrument = 1;

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

// Generated up front so generation cost never lands inside the timed
// region. Mirrors the mix used by the test suite.
std::vector<Command> makeCommands(std::uint64_t seed, std::size_t count) {
    Lcg rng(seed);
    std::vector<Command> cmds;
    cmds.reserve(count);
    std::vector<OrderId> live;
    OrderId nextId = 1;

    for (std::size_t i = 0; i < count; ++i) {
        const std::uint64_t roll = rng.inRange(0, 99);
        if (roll < 15 && !live.empty()) {
            const auto idx = static_cast<std::size_t>(rng.inRange(0, live.size() - 1));
            cmds.push_back(CancelOrder{live[idx]});
            // Swap and pop rather than erase: the order of the live set
            // is an implementation detail of the generator, and an O(n)
            // erase here would dominate the setup time at a million
            // commands. Still fully deterministic.
            live[idx] = live.back();
            live.pop_back();
            continue;
        }
        NewOrder o;
        o.id = nextId++;
        o.instrument = kInstrument;
        o.side = (rng.inRange(0, 1) == 0) ? Side::Buy : Side::Sell;
        o.quantity = rng.inRange(1, 50);
        if (roll >= 92) {
            o.type = OrderType::Market;
            o.tif = TimeInForce::ImmediateOrCancel;
            o.price = 0;
        } else {
            o.type = OrderType::Limit;
            o.price = static_cast<Price>(rng.inRange(9950, 10050));
            o.tif = (roll >= 85) ? TimeInForce::ImmediateOrCancel : TimeInForce::GoodTillCancel;
            if (o.tif == TimeInForce::GoodTillCancel) {
                live.push_back(o.id);
            }
        }
        cmds.push_back(o);
    }
    return cmds;
}

std::uint64_t percentile(std::vector<std::uint64_t>& sorted, double p) {
    if (sorted.empty()) {
        return 0;
    }
    const auto idx = static_cast<std::size_t>(p * static_cast<double>(sorted.size() - 1));
    return sorted[idx];
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t warmup = 100000;
    std::size_t events = 1000000;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--warmup") == 0 && i + 1 < argc) {
            warmup = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--commands") == 0 && i + 1 < argc) {
            events = std::strtoull(argv[++i], nullptr, 10);
        } else {
            std::fprintf(stderr, "usage: %s [--warmup N] [--commands N]\n", argv[0]);
            return 2;
        }
    }

    std::printf("sententia engine_bench\n");
    std::printf("warmup=%zu\n", warmup);
    std::printf("commands=%zu\n", events);

    const std::vector<Command> warmupCmds = makeCommands(0x51EEDULL, warmup);
    const std::vector<Command> timedCmds = makeCommands(0xC0FFEEULL, events);

    MatchingEngine engine(kInstrument);
    EventList sink;
    sink.reserve(4096);

    // Warmup: fills caches, grows the book to a realistic depth, and
    // lets the branch predictor settle, so cold-start noise stays out of
    // the measured percentiles.
    for (const Command& c : warmupCmds) {
        engine.apply(c, sink);
        sink.clear();
    }

    std::vector<std::uint64_t> latencies;
    latencies.reserve(timedCmds.size());

    std::uint64_t eventCount = 0;
    std::uint64_t tradeCount = 0;

    const auto start = std::chrono::steady_clock::now();
    for (const Command& c : timedCmds) {
        const auto t0 = std::chrono::steady_clock::now();
        engine.apply(c, sink);
        const auto t1 = std::chrono::steady_clock::now();
        latencies.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
        eventCount += sink.size();
        for (const Event& e : sink) {
            if (std::holds_alternative<Trade>(e)) {
                ++tradeCount;
            }
        }
        sink.clear();
    }
    const auto end = std::chrono::steady_clock::now();

    const auto wallNs = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());

    std::sort(latencies.begin(), latencies.end());

    std::printf("events=%llu\n", static_cast<unsigned long long>(eventCount));
    std::printf("trades=%llu\n", static_cast<unsigned long long>(tradeCount));
    std::printf("resting_orders=%zu\n", engine.book().orderCount());
    std::printf("throughput_cmds_per_sec=%.0f\n",
                static_cast<double>(timedCmds.size()) / (wallNs / 1e9));
    std::printf("latency_ns: p50=%llu p95=%llu p99=%llu p999=%llu max=%llu\n",
                static_cast<unsigned long long>(percentile(latencies, 0.50)),
                static_cast<unsigned long long>(percentile(latencies, 0.95)),
                static_cast<unsigned long long>(percentile(latencies, 0.99)),
                static_cast<unsigned long long>(percentile(latencies, 0.999)),
                static_cast<unsigned long long>(latencies.back()));
    // Printed so a benchmark run doubles as a determinism check: the
    // same arguments must always print the same checksum.
    std::printf("state_checksum=%llu\n", static_cast<unsigned long long>(engine.stateChecksum()));
    return 0;
}
