// The election state machine, one rule at a time.
//
// These are the unit tests: small, hand-built scenarios where the exact
// expected behaviour is written down. The large randomised split-brain
// work is in test_election_sim.cpp.
#include "sententia/consensus/election.hpp"

#include <vector>

#include "harness.hpp"

using namespace sententia;
using namespace sententia::consensus;
using sententia::net::NodeId;

namespace {

ElectionConfig cfg(std::uint64_t seed = 1) {
    ElectionConfig c;
    c.heartbeatInterval = 50;
    c.electionTimeoutMin = 150;
    c.electionTimeoutMax = 300;
    c.randomSeed = seed;
    return c;
}

Election makeNode(NodeId self, std::vector<NodeId> peers, std::uint64_t seed = 1) {
    return Election(self, std::move(peers), cfg(seed));
}

std::size_t countOf(const ActionList& actions, Action::Kind kind) {
    std::size_t n = 0;
    for (const Action& a : actions) {
        if (a.kind == kind) {
            ++n;
        }
    }
    return n;
}

void testStartsAsFollower() {
    Election node = makeNode(1, {2, 3});
    CHECK(node.role() == NodeRole::Follower);
    CHECK_EQ(node.currentTerm(), Term{0});
    CHECK(!node.leaderId().has_value());
    CHECK(!node.votedFor().has_value());
    // 3 nodes, majority 2. 5 nodes, majority 3. Two disjoint majorities
    // of the same cluster cannot exist, which is the whole basis of the
    // no-split-brain guarantee.
    CHECK_EQ(node.clusterSize(), std::size_t{3});
    CHECK_EQ(node.majority(), std::size_t{2});

    Election five = makeNode(1, {2, 3, 4, 5});
    CHECK_EQ(five.majority(), std::size_t{3});

    Election four = makeNode(1, {2, 3, 4});
    // Even sizes still need a strict majority, so 4 needs 3. An even
    // cluster buys no extra fault tolerance over the odd size below it,
    // which is why odd sizes are the convention.
    CHECK_EQ(four.majority(), std::size_t{3});
}

void testElectionTimeoutStartsAnElection() {
    Election node = makeNode(1, {2, 3});
    ActionList actions;

    // Well before the minimum timeout, nothing happens.
    node.tick(100, actions);
    CHECK(node.role() == NodeRole::Follower);
    CHECK_EQ(actions.size(), std::size_t{0});

    // Past the maximum, an election must have started.
    node.tick(400, actions);
    CHECK(node.role() == NodeRole::Candidate);
    CHECK_EQ(node.currentTerm(), Term{1});
    // Votes for itself.
    CHECK(node.votedFor().has_value());
    CHECK_EQ(node.votedFor().value(), NodeId{1});
    CHECK_EQ(node.votesReceived(), std::size_t{1});
    // And asks everyone else.
    CHECK_EQ(countOf(actions, Action::Kind::SendRequestVote), std::size_t{2});
}

void testWinningRequiresAMajority() {
    Election node = makeNode(1, {2, 3});
    ActionList actions;
    node.tick(400, actions);
    CHECK(node.role() == NodeRole::Candidate);

    // One vote from a peer, plus its own, is 2 of 3. That is a majority.
    actions.clear();
    net::VoteResponse yes;
    yes.term = 1;
    yes.voterId = 2;
    yes.granted = true;
    node.onVoteResponse(yes, 410, actions);

    CHECK(node.role() == NodeRole::Leader);
    CHECK(node.isLeader());
    CHECK_EQ(node.leaderId().value(), NodeId{1});
    CHECK_EQ(countOf(actions, Action::Kind::BecameLeader), std::size_t{1});
    // A new leader asserts itself immediately rather than waiting a
    // heartbeat interval, or followers keep counting down to their own
    // elections.
    CHECK_EQ(countOf(actions, Action::Kind::SendHeartbeat), std::size_t{2});
}

void testMajorityIsNotReachedByOneVoteInFive() {
    Election node = makeNode(1, {2, 3, 4, 5});
    ActionList actions;
    node.tick(400, actions);
    CHECK(node.role() == NodeRole::Candidate);

    net::VoteResponse yes;
    yes.term = 1;
    yes.voterId = 2;
    yes.granted = true;
    node.onVoteResponse(yes, 410, actions);
    // 2 of 5 is not a majority.
    CHECK(node.role() == NodeRole::Candidate);

    yes.voterId = 3;
    node.onVoteResponse(yes, 420, actions);
    // 3 of 5 is.
    CHECK(node.role() == NodeRole::Leader);
}

void testDuplicateVotesDoNotCount() {
    // A retransmitted or duplicated response must not push a candidate
    // to a false majority from a single voter.
    Election node = makeNode(1, {2, 3, 4, 5});
    ActionList actions;
    node.tick(400, actions);

    net::VoteResponse yes;
    yes.term = 1;
    yes.voterId = 2;
    yes.granted = true;
    node.onVoteResponse(yes, 410, actions);
    node.onVoteResponse(yes, 411, actions);
    node.onVoteResponse(yes, 412, actions);

    CHECK_EQ(node.votesReceived(), std::size_t{2});  // self plus node 2
    CHECK(node.role() == NodeRole::Candidate);
}

void testOneVotePerTerm() {
    // The rule that makes two leaders in one term impossible.
    Election voter = makeNode(1, {2, 3});
    ActionList actions;

    net::RequestVote fromTwo;
    fromTwo.term = 1;
    fromTwo.candidateId = 2;
    fromTwo.lastLogSeq = 0;
    voter.onRequestVote(fromTwo, 100, actions);
    CHECK_EQ(actions.size(), std::size_t{1});
    CHECK(actions[0].voteGranted);
    CHECK_EQ(voter.votedFor().value(), NodeId{2});

    // A second candidate in the same term is refused.
    actions.clear();
    net::RequestVote fromThree;
    fromThree.term = 1;
    fromThree.candidateId = 3;
    fromThree.lastLogSeq = 0;
    voter.onRequestVote(fromThree, 110, actions);
    CHECK_EQ(actions.size(), std::size_t{1});
    CHECK(!actions[0].voteGranted);

    // A new term resets the vote, so the same node can vote again.
    actions.clear();
    fromThree.term = 2;
    voter.onRequestVote(fromThree, 120, actions);
    CHECK(actions[0].voteGranted);
    CHECK_EQ(voter.currentTerm(), Term{2});
    CHECK_EQ(voter.votedFor().value(), NodeId{3});

    // Asking twice in the same term from the same candidate is fine,
    // since retransmission is normal.
    actions.clear();
    voter.onRequestVote(fromThree, 130, actions);
    CHECK(actions[0].voteGranted);
}

void testHigherTermAlwaysWins() {
    // The single rule that makes a stale leader harmless.
    Election node = makeNode(1, {2, 3});
    ActionList actions;
    node.tick(400, actions);
    actions.clear();
    net::VoteResponse yes;
    yes.term = 1;
    yes.voterId = 2;
    yes.granted = true;
    node.onVoteResponse(yes, 410, actions);
    CHECK(node.isLeader());
    CHECK_EQ(node.currentTerm(), Term{1});

    // A message from a later term makes it step down, whatever it was.
    actions.clear();
    node.onAppendEntries(5, 3, 500, actions);
    CHECK(node.role() == NodeRole::Follower);
    CHECK_EQ(node.currentTerm(), Term{5});
    CHECK_EQ(node.leaderId().value(), NodeId{3});
    CHECK_EQ(node.stats().stepDowns, std::uint64_t{1});

    // And a message from an earlier term is ignored entirely.
    const Term before = node.currentTerm();
    actions.clear();
    node.onAppendEntries(2, 2, 510, actions);
    CHECK_EQ(node.currentTerm(), before);
    CHECK_EQ(node.leaderId().value(), NodeId{3});
}

void testStaleLeaderStepsDownOnResponse() {
    // A leader partitioned away comes back and learns from a follower's
    // reply that the world moved on. No explicit partition detection is
    // needed: the term does all the work.
    Election node = makeNode(1, {2, 3});
    ActionList actions;
    node.tick(400, actions);
    net::VoteResponse yes;
    yes.term = 1;
    yes.voterId = 2;
    yes.granted = true;
    node.onVoteResponse(yes, 410, actions);
    CHECK(node.isLeader());

    actions.clear();
    node.onAppendResponse(9, 500, actions);
    CHECK(node.role() == NodeRole::Follower);
    CHECK_EQ(node.currentTerm(), Term{9});
    CHECK_EQ(countOf(actions, Action::Kind::SteppedDown), std::size_t{1});
}

void testCandidateConcedesToALeader() {
    Election node = makeNode(1, {2, 3});
    ActionList actions;
    node.tick(400, actions);
    CHECK(node.role() == NodeRole::Candidate);
    CHECK_EQ(node.currentTerm(), Term{1});

    // A leader in the same term means the election is already decided.
    actions.clear();
    node.onAppendEntries(1, 2, 410, actions);
    CHECK(node.role() == NodeRole::Follower);
    CHECK_EQ(node.leaderId().value(), NodeId{2});
}

void testElectionRestrictionRefusesABehindCandidate() {
    // A candidate whose log is behind ours must not win, because a
    // leader missing already-committed entries would drop them.
    Election voter = makeNode(1, {2, 3});
    voter.setLastLogSeq(100);
    ActionList actions;

    net::RequestVote behind;
    behind.term = 1;
    behind.candidateId = 2;
    behind.lastLogSeq = 50;
    voter.onRequestVote(behind, 100, actions);
    CHECK(!actions[0].voteGranted);
    CHECK_EQ(voter.stats().votesRefusedStaleLog, std::uint64_t{1});
    // Refusing on log grounds must not consume the vote for the term.
    CHECK(!voter.votedFor().has_value());

    // Level with us is acceptable.
    actions.clear();
    net::RequestVote level;
    level.term = 1;
    level.candidateId = 3;
    level.lastLogSeq = 100;
    voter.onRequestVote(level, 110, actions);
    CHECK(actions[0].voteGranted);

    // And ahead of us certainly is.
    Election other = makeNode(4, {1, 2, 3});
    other.setLastLogSeq(10);
    actions.clear();
    net::RequestVote ahead;
    ahead.term = 1;
    ahead.candidateId = 1;
    ahead.lastLogSeq = 999;
    other.onRequestVote(ahead, 100, actions);
    CHECK(actions[0].voteGranted);
}

void testGrantingAVoteBacksOffTheTimer() {
    // A voter that immediately ran its own election would split the vote
    // it just helped, so granting resets the timer.
    Election voter = makeNode(1, {2, 3});
    ActionList actions;
    net::RequestVote rv;
    rv.term = 1;
    rv.candidateId = 2;
    rv.lastLogSeq = 0;
    voter.onRequestVote(rv, 100, actions);
    CHECK(actions[0].voteGranted);

    actions.clear();
    // 200ms after the vote is still inside the minimum timeout window.
    voter.tick(300, actions);
    CHECK(voter.role() == NodeRole::Follower);
}

void testLeaderSendsPeriodicHeartbeats() {
    Election node = makeNode(1, {2, 3});
    ActionList actions;
    node.tick(400, actions);
    net::VoteResponse yes;
    yes.term = 1;
    yes.voterId = 2;
    yes.granted = true;
    node.onVoteResponse(yes, 400, actions);
    CHECK(node.isLeader());

    actions.clear();
    node.tick(420, actions);  // less than one interval later
    CHECK_EQ(countOf(actions, Action::Kind::SendHeartbeat), std::size_t{0});

    actions.clear();
    node.tick(460, actions);  // more than 50ms after the last
    CHECK_EQ(countOf(actions, Action::Kind::SendHeartbeat), std::size_t{2});
}

void testHeartbeatsKeepFollowersQuiet() {
    Election follower = makeNode(1, {2, 3});
    ActionList actions;
    // A heartbeat every 50ms for well past several election timeouts.
    for (Millis t = 50; t <= 3000; t += 50) {
        actions.clear();
        follower.onAppendEntries(1, 2, t, actions);
        follower.tick(t, actions);
        CHECK(follower.role() == NodeRole::Follower);
    }
    CHECK_EQ(follower.currentTerm(), Term{1});
    CHECK_EQ(follower.stats().electionsStarted, std::uint64_t{0});
}

void testSingleNodeClusterElectsItself() {
    Election lonely = makeNode(1, {});
    CHECK_EQ(lonely.majority(), std::size_t{1});
    ActionList actions;
    lonely.tick(400, actions);
    // Its own vote is already a majority of one.
    CHECK(lonely.isLeader());
}

void testRandomisedTimeoutsDiffer() {
    // If every node used the same timeout they would all become
    // candidates at the same instant, split the vote, and repeat. The
    // spread is what breaks the symmetry, so it is worth asserting that
    // there actually is one.
    std::vector<Millis> firstElectionAt;
    for (std::uint64_t seed = 1; seed <= 8; ++seed) {
        Election node(static_cast<NodeId>(seed), {2, 3}, cfg(seed * 7919));
        ActionList actions;
        Millis t = 0;
        while (node.role() == NodeRole::Follower && t < 10000) {
            t += 1;
            node.tick(t, actions);
        }
        firstElectionAt.push_back(t);
        // And every timeout must land inside the configured window.
        CHECK(t >= 150);
        CHECK(t <= 301);
    }
    std::size_t distinct = 0;
    for (std::size_t i = 0; i < firstElectionAt.size(); ++i) {
        bool seen = false;
        for (std::size_t j = 0; j < i; ++j) {
            if (firstElectionAt[j] == firstElectionAt[i]) {
                seen = true;
            }
        }
        if (!seen) {
            ++distinct;
        }
    }
    // Not all the same. Anything else means the randomisation is broken.
    CHECK(distinct > 4);
}

void run() {
    testStartsAsFollower();
    testElectionTimeoutStartsAnElection();
    testWinningRequiresAMajority();
    testMajorityIsNotReachedByOneVoteInFive();
    testDuplicateVotesDoNotCount();
    testOneVotePerTerm();
    testHigherTermAlwaysWins();
    testStaleLeaderStepsDownOnResponse();
    testCandidateConcedesToALeader();
    testElectionRestrictionRefusesABehindCandidate();
    testGrantingAVoteBacksOffTheTimer();
    testLeaderSendsPeriodicHeartbeats();
    testHeartbeatsKeepFollowersQuiet();
    testSingleNodeClusterElectsItself();
    testRandomisedTimeoutsDiffer();
}

}  // namespace

TEST_MAIN("test_election")
