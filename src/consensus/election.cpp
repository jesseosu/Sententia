#include "sententia/consensus/election.hpp"

#include <algorithm>

namespace sententia::consensus {

const char* toString(NodeRole r) noexcept {
    switch (r) {
        case NodeRole::Follower:
            return "FOLLOWER";
        case NodeRole::Candidate:
            return "CANDIDATE";
        case NodeRole::Leader:
            return "LEADER";
    }
    return "UNKNOWN";
}

Election::Election(NodeId self, std::vector<NodeId> peers, ElectionConfig config)
    : self_(self), peers_(std::move(peers)), config_(config), rngState_(config.randomSeed) {
    // Seed defensively: a zero seed in an LCG of this shape produces a
    // usable stream, but two nodes sharing a seed would time out in
    // lockstep forever, so mix the node id in as well.
    rngState_ ^= (static_cast<std::uint64_t>(self_) + 0x9E3779B97F4A7C15ULL);
    resetElectionTimer(0);
}

std::uint64_t Election::nextRandom() noexcept {
    rngState_ = rngState_ * 6364136223846793005ULL + 1442695040888963407ULL;
    return rngState_ >> 17;
}

void Election::resetElectionTimer(Millis now) {
    lastHeardFromLeader_ = now;
    const Millis span = config_.electionTimeoutMax - config_.electionTimeoutMin;
    const Millis jitter = span == 0 ? 0 : nextRandom() % (span + 1);
    electionTimeout_ = config_.electionTimeoutMin + jitter;
}

bool Election::observeTerm(Term term, Millis now) {
    if (term <= currentTerm_) {
        return false;
    }
    // The rule that makes a stale leader harmless. Any node seeing a
    // higher term adopts it and reverts to follower, whatever it was
    // doing. A leader that was partitioned away and comes back finds the
    // world has moved on and stands down, without needing to be told.
    const bool wasLeader = role_ == NodeRole::Leader;
    currentTerm_ = term;
    votedFor_.reset();
    role_ = NodeRole::Follower;
    leaderId_.reset();
    votesGranted_.clear();
    stats_.highestTermSeen = std::max(stats_.highestTermSeen, term);
    if (wasLeader) {
        ++stats_.stepDowns;
    }
    resetElectionTimer(now);
    return true;
}

void Election::becomeFollower(Term term, std::optional<NodeId> leader, Millis now) {
    if (role_ == NodeRole::Leader) {
        ++stats_.stepDowns;
    }
    role_ = NodeRole::Follower;
    currentTerm_ = term;
    leaderId_ = leader;
    votesGranted_.clear();
    resetElectionTimer(now);
}

void Election::becomeCandidate(Millis now, ActionList& out) {
    // Every election is for a brand new term. Terms are the logical
    // clock of the protocol: they never go backwards, and they are what
    // lets any two nodes decide whose information is newer without
    // consulting a wall clock they would disagree about anyway.
    ++currentTerm_;
    role_ = NodeRole::Candidate;
    leaderId_.reset();
    votedFor_ = self_;
    votesGranted_.clear();
    // A candidate votes for itself. With one node that is already a
    // majority, so a single-node cluster elects itself immediately.
    votesGranted_.push_back(self_);
    stats_.highestTermSeen = std::max(stats_.highestTermSeen, currentTerm_);
    ++stats_.electionsStarted;
    resetElectionTimer(now);

    if (votesGranted_.size() >= majority()) {
        becomeLeader(now, out);
        return;
    }

    for (const NodeId peer : peers_) {
        Action a;
        a.kind = Action::Kind::SendRequestVote;
        a.to = peer;
        a.term = currentTerm_;
        a.lastLogSeq = lastLogSeq_;
        out.push_back(a);
    }
}

void Election::becomeLeader(Millis now, ActionList& out) {
    role_ = NodeRole::Leader;
    leaderId_ = self_;
    ++stats_.electionsWon;

    Action notify;
    notify.kind = Action::Kind::BecameLeader;
    notify.to = self_;
    notify.term = currentTerm_;
    out.push_back(notify);

    // Assert leadership immediately rather than waiting a heartbeat
    // interval. Until followers hear from the new leader they are still
    // counting down to their own elections.
    lastHeartbeatSent_ = now;
    broadcastHeartbeat(now, out);
}

void Election::broadcastHeartbeat(Millis now, ActionList& out) {
    for (const NodeId peer : peers_) {
        Action a;
        a.kind = Action::Kind::SendHeartbeat;
        a.to = peer;
        a.term = currentTerm_;
        out.push_back(a);
        ++stats_.heartbeatsSent;
    }
    lastHeartbeatSent_ = now;
}

void Election::tick(Millis now, ActionList& out) {
    if (role_ == NodeRole::Leader) {
        if (now - lastHeartbeatSent_ >= config_.heartbeatInterval) {
            broadcastHeartbeat(now, out);
        }
        return;
    }

    // Followers and candidates both run the election timer. A candidate
    // timing out means the election was inconclusive, most likely a
    // split vote, and it simply tries again in a higher term with a
    // freshly randomised timeout. That re-randomisation is what makes
    // repeated splits vanishingly unlikely rather than possible forever.
    if (now - lastHeardFromLeader_ >= electionTimeout_) {
        becomeCandidate(now, out);
    }
}

void Election::onRequestVote(const net::RequestVote& msg, Millis now, ActionList& out) {
    observeTerm(msg.term, now);

    bool granted = false;

    if (msg.term < currentTerm_) {
        // A candidate from the past. Refusing and returning our term
        // tells it to give up and catch up.
        granted = false;
    } else if (votedFor_.has_value() && votedFor_.value() != msg.candidateId) {
        // At most one vote per node per term. THIS is what makes two
        // leaders in one term impossible: two majorities of the same
        // cluster must overlap in at least one node, and that node
        // cannot have voted for both.
        granted = false;
    } else if (msg.lastLogSeq < lastLogSeq_) {
        // The election restriction. A candidate behind us must not win,
        // because a leader missing entries that were already committed
        // elsewhere would silently drop them. Majority voting alone does
        // not prevent this; only this check does.
        granted = false;
        ++stats_.votesRefusedStaleLog;
    } else {
        granted = true;
        votedFor_ = msg.candidateId;
        // Granting a vote means believing an election is genuinely under
        // way, so back off rather than immediately running our own and
        // splitting the vote we just helped.
        resetElectionTimer(now);
    }

    if (granted) {
        ++stats_.votesGranted;
    } else {
        ++stats_.votesRefused;
    }

    Action a;
    a.kind = Action::Kind::SendVoteResponse;
    a.to = msg.candidateId;
    a.term = currentTerm_;
    a.voteGranted = granted;
    out.push_back(a);
}

void Election::onVoteResponse(const net::VoteResponse& msg, Millis now, ActionList& out) {
    if (observeTerm(msg.term, now)) {
        ++stats_.electionsLostToHigherTerm;
        return;
    }
    if (role_ != NodeRole::Candidate || msg.term != currentTerm_) {
        // A vote for an election we are no longer running. Votes are
        // scoped to a term precisely so late replies from an old one
        // cannot be counted towards a new one.
        return;
    }
    if (!msg.granted) {
        return;
    }
    // Idempotent: a duplicated response must not count twice, or a
    // candidate could reach a false majority from a single voter.
    if (std::find(votesGranted_.begin(), votesGranted_.end(), msg.voterId) != votesGranted_.end()) {
        return;
    }
    votesGranted_.push_back(msg.voterId);

    if (votesGranted_.size() >= majority()) {
        becomeLeader(now, out);
    }
}

void Election::onAppendEntries(Term term, NodeId leaderId, Millis now, ActionList& out) {
    if (term < currentTerm_) {
        // A leader from a previous term. Ignore it entirely; its own
        // AppendResponse handling will teach it that it has been
        // superseded.
        return;
    }
    observeTerm(term, now);

    const bool wasLeader = role_ == NodeRole::Leader;
    if (role_ != NodeRole::Follower) {
        // A candidate that hears from a leader of its own term or later
        // concedes. Two candidates cannot both win, and continuing would
        // only prolong the election.
        becomeFollower(term, leaderId, now);
        if (wasLeader) {
            Action a;
            a.kind = Action::Kind::SteppedDown;
            a.to = self_;
            a.term = term;
            out.push_back(a);
        }
        return;
    }

    leaderId_ = leaderId;
    // The heartbeat that keeps this follower from starting an election.
    resetElectionTimer(now);
}

void Election::onAppendResponse(Term term, Millis now, ActionList& out) {
    if (observeTerm(term, now)) {
        // A follower told us it is in a later term, so we are a stale
        // leader. Standing down here is what resolves the tail of a
        // partition without any explicit partition detection.
        Action a;
        a.kind = Action::Kind::SteppedDown;
        a.to = self_;
        a.term = term;
        out.push_back(a);
    }
}

void Election::resetVolatileState() noexcept {
    role_ = NodeRole::Follower;
    leaderId_.reset();
    votesGranted_.clear();
    // Term and votedFor survive a real crash only if they were written
    // to disk. There is no disk yet, so a restarted node forgets both.
    // The consequence is documented in docs/consensus.md rather than
    // hidden: it can vote twice in what it thinks are different terms.
    currentTerm_ = 0;
    votedFor_.reset();
    lastHeardFromLeader_ = 0;
    lastHeartbeatSent_ = 0;
}

}  // namespace sententia::consensus
