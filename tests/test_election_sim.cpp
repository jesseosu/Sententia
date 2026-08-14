// Split-brain, and the schedules that would cause it.
//
// This is the test the whole phase exists for.
//
// Split-brain is two nodes both believing they lead, both accepting
// orders, both producing an authoritative book. It is the catastrophe of
// leader election, and it appears only under specific interleavings of
// timeouts, partitions and crashes. You cannot find it by starting three
// processes and unplugging a network cable, because you will never
// happen to unplug it at the microsecond that matters.
//
// So the whole cluster runs in one process on a virtual clock, and the
// invariant is checked after every single message delivery and every
// single tick, across thousands of randomised schedules. Each schedule
// comes from one seed, so any failure replays exactly.
//
// THE INVARIANT: no term ever has two leaders.
//
// Note what is deliberately *not* the invariant. "At most one leader at
// any instant" is too strong and would fail on correct behaviour: a
// leader cut off by a partition does not know it has been replaced, so
// for a while there really are two nodes calling themselves leader. That
// is not split-brain, because they are in different terms and the older
// one cannot reach a majority, so it cannot commit anything. Getting
// this distinction right is most of understanding why the protocol works.
#include "harness.hpp"
#include "sim.hpp"

using namespace sim;

namespace {

// Fails loudly if any term ever had two leaders.
void assertNoSplitBrain(const Cluster& cluster) {
    const auto offending = cluster.leadership().firstTermWithTwoLeaders();
    if (offending.has_value()) {
        std::fprintf(stderr, "SPLIT BRAIN in term %llu (seed %llu)\n",
                     static_cast<unsigned long long>(offending.value()),
                     static_cast<unsigned long long>(cluster.seed()));
    }
    CHECK(!offending.has_value());
}

void testQuietClusterElectsExactlyOneLeader() {
    for (std::uint64_t seed = 1; seed <= 30; ++seed) {
        Cluster cluster(3, seed);
        cluster.run(3000);
        const auto leaders = cluster.currentLeaders();
        CHECK_EQ(leaders.size(), std::size_t{1});
        assertNoSplitBrain(cluster);
        // A healthy cluster should settle, not hold election after
        // election. A handful of terms is fine; hundreds means the
        // timeouts are misconfigured relative to the heartbeat.
        CHECK(cluster.leadership().termsWithALeader() < 5);
    }
}

void testFiveNodeClusterAlsoSettles() {
    for (std::uint64_t seed = 100; seed <= 130; ++seed) {
        Cluster cluster(5, seed);
        cluster.run(3000);
        CHECK_EQ(cluster.currentLeaders().size(), std::size_t{1});
        assertNoSplitBrain(cluster);
    }
}

void testLeaderFailureTriggersReElection() {
    for (std::uint64_t seed = 200; seed <= 220; ++seed) {
        Cluster cluster(3, seed);
        cluster.run(2000);
        auto leaders = cluster.currentLeaders();
        CHECK_EQ(leaders.size(), std::size_t{1});
        if (leaders.empty()) {
            continue;
        }
        const NodeId dead = leaders[0];
        const Millis killedAt = cluster.now();

        cluster.crash(dead);

        // A new leader must appear, and within a bounded time. The bound
        // is what makes this a liveness property rather than a hope.
        Millis electedAt = 0;
        for (int i = 0; i < 400 && electedAt == 0; ++i) {
            cluster.step(5);
            for (const NodeId id : cluster.currentLeaders()) {
                if (id != dead) {
                    electedAt = cluster.now();
                }
            }
        }
        CHECK(electedAt != 0);
        // Generous, but finite: roughly an election timeout plus a
        // round of voting.
        CHECK(electedAt - killedAt < Millis{1500});

        const auto after = cluster.currentLeaders();
        CHECK_EQ(after.size(), std::size_t{1});
        if (!after.empty()) {
            CHECK(after[0] != dead);
        }
        assertNoSplitBrain(cluster);
    }
}

void testMinorityPartitionCannotElectALeader() {
    // The quorum rule protecting the system, demonstrated directly.
    // Partition 5 nodes into 3 and 2. The side of 2 can never reach 3
    // votes, so it can never elect anyone, however long it tries.
    //
    // The first version of this test asserted "no node on the minority
    // side is a leader" and failed, because sometimes the pre-partition
    // leader lands in the minority and stays leader in its OLD term
    // until it hears otherwise. That is correct behaviour, not the
    // minority electing anybody, and conflating the two is the exact
    // confusion this phase is about. So the partition is now built so
    // the existing leader is on the majority side, and the claim under
    // test is the sharp one: the minority elects nobody, ever.
    for (std::uint64_t seed = 300; seed <= 315; ++seed) {
        Cluster cluster(5, seed);
        cluster.run(2000);
        const auto leaders = cluster.currentLeaders();
        CHECK_EQ(leaders.size(), std::size_t{1});
        if (leaders.size() != 1) {
            continue;
        }
        const NodeId leader = leaders[0];

        // Majority side contains the current leader; minority is two
        // other nodes.
        std::set<NodeId> majority{leader};
        std::set<NodeId> minority;
        for (NodeId id = 1; id <= 5; ++id) {
            if (id == leader) {
                continue;
            }
            if (majority.size() < 3) {
                majority.insert(id);
            } else {
                minority.insert(id);
            }
        }
        CHECK_EQ(majority.size(), std::size_t{3});
        CHECK_EQ(minority.size(), std::size_t{2});

        const Term termAtPartition = cluster.node(leader).election->currentTerm();
        cluster.setPartition({majority, minority});
        cluster.run(6000);

        // The majority side still has a leader, and it is the same one:
        // it never lost contact with a quorum.
        bool majorityHasLeader = false;
        for (const NodeId id : cluster.currentLeaders()) {
            if (majority.count(id)) {
                majorityHasLeader = true;
            }
        }
        CHECK(majorityHasLeader);

        // The minority elected nobody. Not one of them ever reached
        // leader, because two votes is not three.
        for (const NodeId id : minority) {
            CHECK(cluster.node(id).election->role() != NodeRole::Leader);
            CHECK_EQ(cluster.node(id).election->stats().electionsWon, std::uint64_t{0});
            // They kept trying and kept failing, burning terms well past
            // the term that was current when they were cut off. That is
            // correct: a minority partition gives up AVAILABILITY. It
            // does not get to give up consistency instead.
            CHECK(cluster.node(id).election->currentTerm() > termAtPartition);
            CHECK(cluster.node(id).election->stats().electionsStarted > 0);
        }

        assertNoSplitBrain(cluster);

        // Healing brings them back as followers of whoever leads now,
        // with no explicit reconciliation step anywhere in the protocol.
        cluster.healPartition();
        cluster.run(6000);
        CHECK_EQ(cluster.currentLeaders().size(), std::size_t{1});
        assertNoSplitBrain(cluster);
    }
}

void testPartitionedOldLeaderStepsDownOnHealing() {
    // Isolate the leader from everyone. The majority elects a
    // replacement in a higher term. The old leader still thinks it
    // leads, which is fine because it can commit nothing. When the
    // partition heals it must discover the higher term and stand down.
    for (std::uint64_t seed = 400; seed <= 415; ++seed) {
        Cluster cluster(5, seed);
        cluster.run(2000);
        auto leaders = cluster.currentLeaders();
        if (leaders.size() != 1) {
            continue;
        }
        const NodeId old = leaders[0];
        const Term oldTerm = cluster.node(old).election->currentTerm();

        std::set<NodeId> rest;
        for (NodeId id = 1; id <= 5; ++id) {
            if (id != old) {
                rest.insert(id);
            }
        }
        cluster.setPartition({{old}, rest});
        cluster.run(4000);

        // A new leader on the majority side, in a strictly higher term.
        NodeId replacement = 0;
        for (const NodeId id : cluster.currentLeaders()) {
            if (id != old) {
                replacement = id;
            }
        }
        CHECK(replacement != 0);
        if (replacement != 0) {
            CHECK(cluster.node(replacement).election->currentTerm() > oldTerm);
        }
        // Two nodes calling themselves leader right now, in DIFFERENT
        // terms. Not split-brain: the isolated one cannot reach a
        // majority so it cannot commit.
        assertNoSplitBrain(cluster);

        cluster.healPartition();
        cluster.run(4000);

        CHECK(cluster.node(old).election->role() != NodeRole::Leader);
        CHECK_EQ(cluster.currentLeaders().size(), std::size_t{1});
        assertNoSplitBrain(cluster);
    }
}

// The main event: random partitions, random crashes, random message
// loss, invariant checked continuously.
void testChaosNeverProducesSplitBrain(std::size_t nodeCount, std::uint64_t seedBase,
                                      std::uint64_t dropPercent, int rounds) {
    for (int r = 0; r < rounds; ++r) {
        const std::uint64_t seed = seedBase + static_cast<std::uint64_t>(r);
        Cluster cluster(nodeCount, seed);
        cluster.setDropPercent(dropPercent);
        cluster.setLatency(1, 25);
        Lcg rng(seed * 2654435761ULL);

        std::set<NodeId> crashed;

        for (int epoch = 0; epoch < 60; ++epoch) {
            // Randomly partition, crash, restart, or heal.
            const std::uint64_t action = rng.inRange(0, 99);

            if (action < 25) {
                // Random two-way partition.
                std::set<NodeId> a;
                std::set<NodeId> b;
                for (NodeId id = 1; id <= static_cast<NodeId>(nodeCount); ++id) {
                    (rng.chance(50) ? a : b).insert(id);
                }
                if (!a.empty() && !b.empty()) {
                    cluster.setPartition({a, b});
                }
            } else if (action < 40) {
                cluster.healPartition();
            } else if (action < 60) {
                // Crash a node, but never so many that a majority is
                // impossible: with no possible quorum the cluster is
                // correctly unavailable and the test would prove nothing.
                const auto maxCrashed = (nodeCount - 1) / 2;
                if (crashed.size() < maxCrashed) {
                    const auto victim = static_cast<NodeId>(rng.inRange(1, nodeCount));
                    if (!crashed.count(victim)) {
                        cluster.crash(victim);
                        crashed.insert(victim);
                    }
                }
            } else if (action < 75 && !crashed.empty()) {
                const auto it = crashed.begin();
                cluster.restart(*it);
                crashed.erase(it);
            }

            cluster.run(rng.inRange(200, 900));
            // Checked every epoch, not just at the end, so a violation
            // is caught close to the schedule that caused it.
            assertNoSplitBrain(cluster);
        }

        // Finally, let it settle with everything healthy and assert it
        // converges on exactly one leader. Chaos must not leave the
        // cluster permanently leaderless.
        cluster.healPartition();
        for (const NodeId id : crashed) {
            cluster.restart(id);
        }
        cluster.run(8000);
        assertNoSplitBrain(cluster);
        CHECK_EQ(cluster.currentLeaders().size(), std::size_t{1});
    }
}

void run() {
    testQuietClusterElectsExactlyOneLeader();
    testFiveNodeClusterAlsoSettles();
    testLeaderFailureTriggersReElection();
    testMinorityPartitionCannotElectALeader();
    testPartitionedOldLeaderStepsDownOnHealing();

    // Three cluster sizes, three loss rates. Every one of these runs 60
    // epochs of randomised faults with the invariant checked each time.
    testChaosNeverProducesSplitBrain(3, 1000, 0, 25);
    testChaosNeverProducesSplitBrain(3, 2000, 10, 25);
    testChaosNeverProducesSplitBrain(5, 3000, 0, 25);
    testChaosNeverProducesSplitBrain(5, 4000, 15, 25);
    testChaosNeverProducesSplitBrain(7, 5000, 20, 15);
}

}  // namespace

TEST_MAIN("test_election_sim")
