// Chaos: replicate under a link that keeps failing, and assert the
// backup still converges.
//
// The unit tests check that catch-up works when it is invoked cleanly.
// This one refuses to be that polite: it drops the connection at
// unpredictable points in the middle of a command stream, hundreds of
// times, and asserts that the backup ends byte-identical to the primary
// regardless.
//
// The randomness is a seeded LCG, so a failure is replayable. That is
// the same discipline as Phase 1's determinism generator: chaos that
// cannot be reproduced is not a test, it is an anecdote.
#include "sententia/replication/replicator.hpp"

#include <functional>
#include <memory>
#include <vector>

#include "harness.hpp"
#include "support.hpp"

using namespace sententia;
using namespace sententia::net;
using namespace sententia::replication;

namespace {

struct Node {
    MatchingEngine engine{support::kInstrument};
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

    void poll(int timeoutMs = 0) {
        transport.poll(timeoutMs);
        replicator->tick();
    }
};

void runChaos(std::uint64_t seed, std::size_t commandCount, int dropEveryHint) {
    Node primary(1, Role::Primary, ReplicationMode::Asynchronous);
    Node backup(2, Role::Backup, ReplicationMode::Asynchronous);

    // Redial quickly so the run does not spend all its cycles waiting.
    primary.transport.setReconnectDelayCycles(2);
    primary.transport.addPeer(PeerConfig{2, "127.0.0.1", backup.transport.listenPort()});

    for (int i = 0; i < 4000 && primary.transport.readyPeerCount() == 0; ++i) {
        primary.poll(1);
        backup.poll(1);
    }
    CHECK_EQ(primary.transport.readyPeerCount(), std::size_t{1});

    const auto cmds = support::generateCommands(seed, commandCount);
    support::Lcg rng(seed ^ 0x9E3779B97F4A7C15ULL);

    std::size_t next = 0;
    std::size_t drops = 0;

    for (int cycle = 0; cycle < 2000000 && next < cmds.size(); ++cycle) {
        if (primary.replicator->submit(cmds[next]).ok()) {
            ++next;
        }
        primary.poll(0);
        backup.poll(0);

        // Randomly sever the link mid-stream.
        if (rng.inRange(0, static_cast<std::uint64_t>(dropEveryHint)) == 0) {
            if (primary.transport.disconnectPeer(2, "chaos")) {
                ++drops;
            }
        }
    }
    CHECK_EQ(next, cmds.size());
    CHECK(drops > 0);

    // Now let it settle. Convergence is allowed to take a while; it is
    // not allowed to fail to happen.
    bool converged = false;
    for (int i = 0; i < 400000 && !converged; ++i) {
        primary.poll(1);
        backup.poll(1);
        converged = backup.replicator->lastApplied() == primary.log.lastSeq() &&
                    primary.replicator->lastApplied() == primary.log.lastSeq();
    }

    CHECK(converged);
    CHECK_EQ(backup.replicator->lastApplied(), Sequence{cmds.size()});
    // The property that matters: identical state after arbitrary
    // link failure, having never shipped any state.
    CHECK_EQ(backup.engine.stateChecksum(), primary.engine.stateChecksum());
    CHECK_EQ(backup.engine.book().checksum(), primary.engine.book().checksum());
    CHECK_EQ(primary.replicator->stats().checksumMismatches, std::uint64_t{0});
    // Gaps were genuinely detected and repaired, so the recovery path
    // was exercised rather than accidentally avoided.
    CHECK(backup.replicator->stats().gapsDetected > 0);
}

void run() {
    // Several seeds and several drop rates, from occasional to brutal.
    runChaos(0xC4051ULL, 3000, 400);
    runChaos(0xC4052ULL, 3000, 120);
    runChaos(0xC4053ULL, 2000, 40);
    runChaos(0xC4054ULL, 2000, 15);
}

}  // namespace

TEST_MAIN("test_replication_chaos")
