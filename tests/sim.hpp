// Sententia - deterministic cluster simulator for election testing.
//
// Runs a whole cluster of Election state machines in one process, on a
// virtual clock, over a message bus that can delay, drop, partition and
// crash at will. Nothing here touches a socket or a real clock.
//
// Why this exists rather than a multi-process integration test:
//
//   * Split-brain appears only under specific interleavings of timeouts,
//     partitions and crashes. Reproducing those by starting processes
//     and unplugging things is a matter of luck.
//   * Here the entire schedule comes from one seed, so a failing run
//     replays exactly. Chaos you cannot reproduce is an anecdote.
//   * Invariants can be checked after every single delivery, not just at
//     the end. A violation is caught at the step that caused it, with
//     the whole history available.
//   * A simulated hour costs milliseconds, so thousands of schedules fit
//     in a test run.
//
// This is how production Raft implementations are tested, and it is only
// possible because the election state machine has no I/O in it.
#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "sententia/consensus/election.hpp"

namespace sim {

using namespace sententia;
using namespace sententia::consensus;
using sententia::net::NodeId;

// Reused rather than std::mt19937 plus a distribution, because the
// standard library's distributions are not specified to produce the same
// values across implementations, and a schedule that differs by platform
// is not a reproducible schedule.
class Lcg {
public:
    explicit Lcg(std::uint64_t seed) noexcept : state_(seed ? seed : 1) {}
    std::uint64_t next() noexcept {
        state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return state_ >> 17;
    }
    std::uint64_t inRange(std::uint64_t lo, std::uint64_t hi) noexcept {
        return hi <= lo ? lo : lo + next() % (hi - lo + 1);
    }
    bool chance(std::uint64_t percent) noexcept { return inRange(0, 99) < percent; }

private:
    std::uint64_t state_;
};

struct InFlight {
    Millis deliverAt{};
    NodeId from{};
    NodeId to{};
    net::Message message;
    std::uint64_t ordinal{};  // ties broken deterministically
};

struct NodeSim {
    NodeId id{};
    std::unique_ptr<Election> election;
    bool crashed{false};
    // Highest term at which this node was observed to be leader, used by
    // the split-brain invariant.
    std::map<Term, bool> ledInTerm;
};

// A record of who led in each term, across the whole run. The invariant
// that matters is that no term ever has two.
struct LeadershipLog {
    std::map<Term, std::set<NodeId>> leadersByTerm;

    void observe(Term term, NodeId leader) { leadersByTerm[term].insert(leader); }

    // THE property. Two leaders in one term is split-brain.
    std::optional<Term> firstTermWithTwoLeaders() const {
        for (const auto& [term, leaders] : leadersByTerm) {
            if (leaders.size() > 1) {
                return term;
            }
        }
        return std::nullopt;
    }

    std::size_t termsWithALeader() const { return leadersByTerm.size(); }
};

class Cluster {
public:
    Cluster(std::size_t nodeCount, std::uint64_t seed, ElectionConfig base = {})
        : rng_(seed), seed_(seed) {
        std::vector<NodeId> all;
        for (std::size_t i = 0; i < nodeCount; ++i) {
            all.push_back(static_cast<NodeId>(i + 1));
        }
        for (const NodeId id : all) {
            std::vector<NodeId> peers;
            for (const NodeId other : all) {
                if (other != id) {
                    peers.push_back(other);
                }
            }
            ElectionConfig cfg = base;
            // Distinct seeds per node. Sharing one would destroy the
            // symmetry breaking that randomised timeouts exist for.
            cfg.randomSeed = seed ^ (0x1000193ULL * (id + 1));
            auto node = std::make_unique<NodeSim>();
            node->id = id;
            node->election = std::make_unique<Election>(id, peers, cfg);
            nodes_.push_back(std::move(node));
        }
    }

    std::size_t size() const noexcept { return nodes_.size(); }
    Millis now() const noexcept { return now_; }
    const LeadershipLog& leadership() const noexcept { return leadership_; }
    std::uint64_t seed() const noexcept { return seed_; }

    NodeSim& node(NodeId id) { return *nodes_[id - 1]; }
    const NodeSim& node(NodeId id) const { return *nodes_[id - 1]; }

    // Network faults. A partition is expressed as a set of groups; two
    // nodes can exchange messages only if they share a group.
    void setPartition(std::vector<std::set<NodeId>> groups) { partition_ = std::move(groups); }
    void healPartition() { partition_.clear(); }

    bool canReach(NodeId from, NodeId to) const {
        if (partition_.empty()) {
            return true;
        }
        for (const auto& group : partition_) {
            if (group.count(from) && group.count(to)) {
                return true;
            }
        }
        return false;
    }

    void crash(NodeId id) {
        NodeSim& n = node(id);
        n.crashed = true;
        n.election->resetVolatileState();
        // Everything already in flight to or from a crashed node is lost.
        inFlight_.erase(
            std::remove_if(inFlight_.begin(), inFlight_.end(),
                           [id](const InFlight& m) { return m.to == id || m.from == id; }),
            inFlight_.end());
    }

    void restart(NodeId id) { node(id).crashed = false; }

    // Advances the virtual clock by `deltaMs`, delivering everything due
    // and ticking every live node. Invariants are checked continuously.
    void step(Millis deltaMs) {
        now_ += deltaMs;

        // Deliver in (time, ordinal) order so the schedule is a total
        // order and does not depend on container iteration.
        std::sort(inFlight_.begin(), inFlight_.end(), [](const InFlight& a, const InFlight& b) {
            return a.deliverAt != b.deliverAt ? a.deliverAt < b.deliverAt : a.ordinal < b.ordinal;
        });

        std::vector<InFlight> due;
        auto firstUndelivered =
            std::partition_point(inFlight_.begin(), inFlight_.end(),
                                 [this](const InFlight& m) { return m.deliverAt <= now_; });
        due.assign(inFlight_.begin(), firstUndelivered);
        inFlight_.erase(inFlight_.begin(), firstUndelivered);

        for (const InFlight& m : due) {
            deliver(m);
            recordLeadership();
        }

        for (auto& n : nodes_) {
            if (n->crashed) {
                continue;
            }
            ActionList actions;
            n->election->tick(now_, actions);
            dispatch(n->id, actions);
            recordLeadership();
        }
    }

    // Runs for a simulated duration in `stepMs` slices.
    void run(Millis durationMs, Millis stepMs = 5) {
        const Millis end = now_ + durationMs;
        while (now_ < end) {
            step(stepMs);
        }
    }

    std::vector<NodeId> currentLeaders() const {
        std::vector<NodeId> out;
        for (const auto& n : nodes_) {
            if (!n->crashed && n->election->isLeader()) {
                out.push_back(n->id);
            }
        }
        return out;
    }

    // Leaders that agree on the highest term. Under a partition an old
    // leader may not yet have noticed it was replaced, which is not
    // split-brain: they are in different terms and the older one cannot
    // commit anything, because it cannot reach a majority.
    std::vector<NodeId> leadersInHighestTerm() const {
        Term highest = 0;
        for (const auto& n : nodes_) {
            if (!n->crashed && n->election->isLeader()) {
                highest = std::max(highest, n->election->currentTerm());
            }
        }
        std::vector<NodeId> out;
        if (highest == 0) {
            return out;
        }
        for (const auto& n : nodes_) {
            if (!n->crashed && n->election->isLeader() && n->election->currentTerm() == highest) {
                out.push_back(n->id);
            }
        }
        return out;
    }

    // Network timing knobs.
    void setLatency(Millis minMs, Millis maxMs) {
        latencyMin_ = minMs;
        latencyMax_ = maxMs;
    }
    void setDropPercent(std::uint64_t percent) { dropPercent_ = percent; }

    std::uint64_t messagesSent() const noexcept { return messagesSent_; }
    std::uint64_t messagesDropped() const noexcept { return messagesDropped_; }

private:
    void recordLeadership() {
        for (const auto& n : nodes_) {
            if (!n->crashed && n->election->isLeader()) {
                leadership_.observe(n->election->currentTerm(), n->id);
            }
        }
    }

    void send(NodeId from, NodeId to, net::Message msg) {
        ++messagesSent_;
        if (node(from).crashed || node(to).crashed || !canReach(from, to)) {
            ++messagesDropped_;
            return;
        }
        if (dropPercent_ > 0 && rng_.chance(dropPercent_)) {
            ++messagesDropped_;
            return;
        }
        InFlight m;
        m.from = from;
        m.to = to;
        m.message = std::move(msg);
        m.deliverAt = now_ + rng_.inRange(latencyMin_, latencyMax_);
        m.ordinal = ++ordinalCounter_;
        inFlight_.push_back(std::move(m));
    }

    void dispatch(NodeId from, const ActionList& actions) {
        for (const Action& a : actions) {
            switch (a.kind) {
                case Action::Kind::SendRequestVote: {
                    net::RequestVote rv;
                    rv.term = a.term;
                    rv.candidateId = from;
                    rv.lastLogSeq = a.lastLogSeq;
                    send(from, a.to, net::Message{rv});
                    break;
                }
                case Action::Kind::SendVoteResponse: {
                    net::VoteResponse vr;
                    vr.term = a.term;
                    vr.voterId = from;
                    vr.granted = a.voteGranted;
                    send(from, a.to, net::Message{vr});
                    break;
                }
                case Action::Kind::SendHeartbeat: {
                    net::AppendEntries ae;
                    ae.term = a.term;
                    ae.leaderId = from;
                    send(from, a.to, net::Message{ae});
                    break;
                }
                case Action::Kind::BecameLeader:
                case Action::Kind::SteppedDown:
                    break;
            }
        }
    }

    void deliver(const InFlight& m) {
        NodeSim& target = node(m.to);
        if (target.crashed) {
            return;
        }
        ActionList actions;
        std::visit(
            [&](const auto& msg) {
                using T = std::decay_t<decltype(msg)>;
                if constexpr (std::is_same_v<T, net::RequestVote>) {
                    target.election->onRequestVote(msg, now_, actions);
                } else if constexpr (std::is_same_v<T, net::VoteResponse>) {
                    target.election->onVoteResponse(msg, now_, actions);
                } else if constexpr (std::is_same_v<T, net::AppendEntries>) {
                    target.election->onAppendEntries(msg.term, msg.leaderId, now_, actions);
                    // Followers answer heartbeats, which is how a stale
                    // leader learns its term has been superseded.
                    net::AppendResponse ar;
                    ar.term = target.election->currentTerm();
                    ar.nodeId = target.id;
                    ar.ok = true;
                    const_cast<Cluster*>(this)->send(target.id, msg.leaderId, net::Message{ar});
                } else if constexpr (std::is_same_v<T, net::AppendResponse>) {
                    target.election->onAppendResponse(msg.term, now_, actions);
                }
            },
            m.message);
        dispatch(m.to, actions);
    }

    std::vector<std::unique_ptr<NodeSim>> nodes_;
    std::vector<InFlight> inFlight_;
    std::vector<std::set<NodeId>> partition_;
    LeadershipLog leadership_;
    Lcg rng_;
    std::uint64_t seed_{};
    Millis now_{0};
    Millis latencyMin_{1};
    Millis latencyMax_{10};
    std::uint64_t dropPercent_{0};
    std::uint64_t ordinalCounter_{0};
    std::uint64_t messagesSent_{0};
    std::uint64_t messagesDropped_{0};
};

}  // namespace sim
