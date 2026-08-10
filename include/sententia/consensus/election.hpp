// Sententia - Raft-style leader election.
//
// Until now the primary was designated by hand: I decided which node led
// and told it so. Now the cluster decides for itself, notices when the
// leader dies, and replaces it without anyone logging in at 3am.
//
// THE SHAPE OF THIS FILE IS THE POINT.
//
// Election is where the nastiest distributed-systems bugs live, and the
// nastiest of all is split-brain: two nodes both believing they lead,
// both accepting orders, both producing an authoritative book. Bugs like
// that appear only under specific interleavings of timeouts, partitions
// and crashes, which is exactly what you cannot reproduce by running two
// processes and unplugging things.
//
// So this is a pure state machine. It owns no socket, reads no clock,
// and starts no thread. Time arrives as a parameter. Messages arrive as
// function calls. Everything it wants done comes back as a list of
// Actions for the caller to perform.
//
// That means an entire cluster can be simulated in one process, with a
// virtual clock, and its invariants checked after every single step,
// across thousands of randomised partition and crash schedules. The
// split-brain test is not a hopeful integration test. It is a proof by
// exhaustion over a large space of schedules, and it is reproducible
// because the schedule comes from a seed.
//
// This is the same discipline as Phase 1 (no clock in the engine) and
// Phase 2 (no sockets in the framing layer), applied to the hardest
// component in the project.
//
// ON RANDOMNESS, WHICH LOOKS LIKE A CONTRADICTION.
//
// Phase 1 banned randomness from the matching engine because replicas
// must agree on its output. Election *requires* randomness: if every
// follower timed out at the same instant they would all become
// candidates together, split the vote, and repeat forever. Randomised
// timeouts break that symmetry.
//
// The two are not in conflict, they are opposite requirements of
// different state machines. The matching engine must be deterministic so
// replicas agree. The election must be randomised so replicas disagree
// about when to act. Here the randomness is a seeded LCG owned by the
// node, so a given seed replays exactly.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "sententia/net/message.hpp"

namespace sententia::consensus {

using sententia::net::NodeId;

// Logical milliseconds. Supplied by the caller, never read from a clock
// in here. In production it comes from steady_clock at the edge; in the
// simulator it is whatever the test says it is.
using Millis = std::uint64_t;
using Term = std::uint64_t;

enum class NodeRole {
    Follower,
    Candidate,
    Leader,
};

const char* toString(NodeRole r) noexcept;

struct ElectionConfig {
    // How often a leader reasserts itself. Must be comfortably shorter
    // than the minimum election timeout, or followers will time out on a
    // perfectly healthy leader and unseat it for no reason.
    Millis heartbeatInterval{50};

    // Randomised per election, uniformly in [min, max]. The spread is
    // what breaks symmetry between followers. Too narrow and split votes
    // repeat; too wide and failover is needlessly slow.
    Millis electionTimeoutMin{150};
    Millis electionTimeoutMax{300};

    // Seeds this node's timeout randomisation. Two nodes must not share
    // a seed, or they lose the symmetry breaking entirely and time out
    // in lockstep forever.
    std::uint64_t randomSeed{1};
};

// Something the state machine wants done. It never does these itself.
struct Action {
    enum class Kind {
        SendRequestVote,   // to `to`
        SendVoteResponse,  // to `to`
        SendHeartbeat,     // to `to`
        BecameLeader,      // notify the application
        SteppedDown,       // notify the application
    };

    Kind kind{};
    NodeId to{};
    Term term{};
    // SendRequestVote
    std::uint64_t lastLogSeq{};
    // SendVoteResponse
    bool voteGranted{false};
};

using ActionList = std::vector<Action>;

struct ElectionStats {
    std::uint64_t electionsStarted{0};
    std::uint64_t electionsWon{0};
    std::uint64_t electionsLostToHigherTerm{0};
    std::uint64_t votesGranted{0};
    std::uint64_t votesRefused{0};
    std::uint64_t votesRefusedStaleLog{0};
    std::uint64_t stepDowns{0};
    std::uint64_t heartbeatsSent{0};
    Term highestTermSeen{0};
};

class Election {
public:
    // `peers` excludes self. Cluster size is peers + 1, and a majority
    // is computed from that.
    Election(NodeId self, std::vector<NodeId> peers, ElectionConfig config);

    // Advances time. Emits heartbeats when leading, and starts an
    // election when a follower or candidate has waited long enough.
    void tick(Millis now, ActionList& out);

    void onRequestVote(const net::RequestVote& msg, Millis now, ActionList& out);
    void onVoteResponse(const net::VoteResponse& msg, Millis now, ActionList& out);

    // Any AppendEntries acts as a heartbeat: it is how a leader asserts
    // it is still alive. Raft does the same, and unifying them means
    // there is no separate liveness path that could disagree with the
    // replication path about who leads.
    void onAppendEntries(Term term, NodeId leaderId, Millis now, ActionList& out);
    void onAppendResponse(Term term, Millis now, ActionList& out);

    // The local log head, used for the election restriction. The
    // application supplies it because the election does not own the log.
    void setLastLogSeq(std::uint64_t seq) noexcept { lastLogSeq_ = seq; }
    std::uint64_t lastLogSeq() const noexcept { return lastLogSeq_; }

    NodeRole role() const noexcept { return role_; }
    Term currentTerm() const noexcept { return currentTerm_; }
    bool isLeader() const noexcept { return role_ == NodeRole::Leader; }
    std::optional<NodeId> leaderId() const noexcept { return leaderId_; }
    std::optional<NodeId> votedFor() const noexcept { return votedFor_; }
    NodeId self() const noexcept { return self_; }

    std::size_t clusterSize() const noexcept { return peers_.size() + 1; }
    // Strict majority: more than half. For 3 nodes that is 2, for 5 it
    // is 3. Two disjoint majorities cannot exist, which is the entire
    // reason split-brain is impossible.
    std::size_t majority() const noexcept { return clusterSize() / 2 + 1; }
    std::size_t votesReceived() const noexcept { return votesGranted_.size(); }

    const ElectionStats& stats() const noexcept { return stats_; }

    // Wipes volatile state as though the process had restarted. Term and
    // vote would be on disk in a durable implementation; here the
    // simulator uses this to model a crash, and the loss of `votedFor`
    // it causes is called out honestly in docs/consensus.md.
    void resetVolatileState() noexcept;

private:
    void becomeFollower(Term term, std::optional<NodeId> leader, Millis now);
    void becomeCandidate(Millis now, ActionList& out);
    void becomeLeader(Millis now, ActionList& out);
    void resetElectionTimer(Millis now);
    void broadcastHeartbeat(Millis now, ActionList& out);
    // Adopts a strictly higher term and steps down. Returns true if it
    // did. This one rule is what makes a stale leader harmless.
    bool observeTerm(Term term, Millis now);
    std::uint64_t nextRandom() noexcept;

    NodeId self_{};
    std::vector<NodeId> peers_;
    ElectionConfig config_;

    NodeRole role_{NodeRole::Follower};
    Term currentTerm_{0};
    std::optional<NodeId> votedFor_;
    std::optional<NodeId> leaderId_;
    std::uint64_t lastLogSeq_{0};

    Millis lastHeardFromLeader_{0};
    Millis electionTimeout_{0};
    Millis lastHeartbeatSent_{0};

    std::vector<NodeId> votesGranted_;
    std::uint64_t rngState_{0};
    ElectionStats stats_;
};

}  // namespace sententia::consensus
