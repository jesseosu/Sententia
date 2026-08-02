// Sententia - synchronous versus asynchronous replication.
//
// Makes the consistency/latency tradeoff a number rather than an
// assertion. Both modes replicate the identical command stream between
// two in-process nodes over loopback TCP, and both end at the same state
// checksum. The difference is what it costs to get there.
//
// The clock lives here, at the edge, never inside the engine or the
// replicator. Same rule as Phase 1's benchmark.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "sententia/replication/replicator.hpp"

using namespace sententia;
using namespace sententia::net;
using namespace sententia::replication;

namespace {

constexpr InstrumentId kInstrument = 42;

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
            live[idx] = live.back();
            live.pop_back();
            continue;
        }
        NewOrder o;
        o.id = nextId++;
        o.instrument = kInstrument;
        o.side = (rng.inRange(0, 1) == 0) ? Side::Buy : Side::Sell;
        o.quantity = rng.inRange(1, 50);
        o.type = OrderType::Limit;
        o.price = static_cast<Price>(rng.inRange(9950, 10050));
        o.tif = TimeInForce::GoodTillCancel;
        live.push_back(o.id);
        cmds.push_back(o);
    }
    return cmds;
}

struct Node {
    MatchingEngine engine{kInstrument};
    CommandLog log;
    Transport transport;
    std::unique_ptr<Replicator> replicator;

    Node(NodeId id, Role role, ReplicationMode mode) : transport(id, 0) {
        std::string error;
        transport.start(error);
        replicator = std::make_unique<Replicator>(id, role, mode, engine, log, transport);
        transport.onMessage(
            [this](NodeId from, const Message& m) { replicator->onMessage(from, m); });
        transport.onPeerUp([this](NodeId p, const std::string&) { replicator->onPeerUp(p); });
        transport.onPeerDown([this](NodeId p, const std::string&) { replicator->onPeerDown(p); });
    }
    void poll() {
        transport.poll(0);
        replicator->tick();
    }
};

std::uint64_t percentile(std::vector<std::uint64_t>& sorted, double p) {
    if (sorted.empty()) {
        return 0;
    }
    const auto idx = static_cast<std::size_t>(p * static_cast<double>(sorted.size() - 1));
    return sorted[idx];
}

void runMode(ReplicationMode mode, std::size_t count, std::size_t window) {
    Node primary(1, Role::Primary, mode);
    Node backup(2, Role::Backup, mode);
    primary.replicator->setSyncWindow(window);
    primary.transport.addPeer(PeerConfig{2, "127.0.0.1", backup.transport.listenPort()});
    for (int i = 0; i < 4000 && primary.transport.readyPeerCount() == 0; ++i) {
        primary.transport.poll(1);
        backup.transport.poll(1);
    }

    const auto cmds = makeCommands(0xB3C4ULL, count);

    std::vector<std::uint64_t> latencies;
    latencies.reserve(count);

    const auto start = std::chrono::steady_clock::now();
    std::size_t next = 0;
    while (next < cmds.size()) {
        const auto t0 = std::chrono::steady_clock::now();
        // Time from offering a command to it being accepted, retries
        // included. Under sync that spans the round trip to the backup,
        // which is exactly the cost the mode buys its guarantee with.
        while (next < cmds.size()) {
            if (primary.replicator->submit(cmds[next]).ok()) {
                break;
            }
            primary.poll();
            backup.poll();
        }
        if (next >= cmds.size()) {
            break;
        }
        const auto t1 = std::chrono::steady_clock::now();
        latencies.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
        ++next;
        primary.poll();
        backup.poll();
    }

    const auto submitDone = std::chrono::steady_clock::now();
    long drainIters = 0;
    // Drain until both sides agree.
    for (int i = 0; i < 400000; ++i) {
        ++drainIters;
        primary.poll();
        backup.poll();
        if (backup.replicator->lastApplied() == primary.log.lastSeq() &&
            primary.replicator->lastApplied() == primary.log.lastSeq()) {
            break;
        }
    }
    const auto end = std::chrono::steady_clock::now();
    const auto wallNs = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());

    const auto submitNs = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(submitDone - start).count());
    const auto drainNs = wallNs - submitNs;
    std::sort(latencies.begin(), latencies.end());
    const auto& rs = primary.replicator->stats();
    const auto& ts = primary.transport.stats();
    // Cost per command, printed alongside speed. A replication scheme
    // that converges correctly while sending a thousand times more
    // than it needs to is not working, and only this line would say so.
    std::printf("    entries/cmd=%.2f  msgs/cmd=%.2f  bytes/cmd=%.1f  busy_retries=%llu\n",
                static_cast<double>(rs.entriesSent) / static_cast<double>(count),
                static_cast<double>(ts.messagesSent) / static_cast<double>(count),
                static_cast<double>(ts.bytesSent) / static_cast<double>(count),
                (unsigned long long)rs.submitRejectedBusy);
    (void)drainIters;
    (void)submitNs;
    (void)drainNs;

    std::printf(
        "mode=%-5s window=%-4zu  throughput=%9.0f cmd/s  submit_ns: p50=%-8llu p99=%-9llu"
        "  converged=%s  checksums_%s\n",
        toString(mode), window, static_cast<double>(count) / (wallNs / 1e9),
        static_cast<unsigned long long>(percentile(latencies, 0.50)),
        static_cast<unsigned long long>(percentile(latencies, 0.99)),
        backup.replicator->lastApplied() == primary.log.lastSeq() ? "yes" : "NO",
        backup.engine.stateChecksum() == primary.engine.stateChecksum() ? "match" : "DIFFER");
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t count = 20000;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--commands") == 0 && i + 1 < argc) {
            count = std::strtoull(argv[++i], nullptr, 10);
        }
    }

    std::printf("sententia replication_bench\ncommands=%zu\n\n", count);
    std::printf(
        "Synchronous: the primary applies a command only once the backup holds it.\n"
        "Asynchronous: the primary applies immediately and the backup follows.\n\n");

    runMode(ReplicationMode::Synchronous, count, 1);
    runMode(ReplicationMode::Synchronous, count, 8);
    runMode(ReplicationMode::Synchronous, count, 64);
    runMode(ReplicationMode::Asynchronous, count, 1);
    return 0;
}
