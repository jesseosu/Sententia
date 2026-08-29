#include "sententia/replication/replicator.hpp"

#include <algorithm>
#include <functional>
#include <vector>

namespace sententia::replication {
namespace {

using sententia::net::SendResult;

}  // namespace

const char* toString(Role r) noexcept {
    switch (r) {
        case Role::Primary:
            return "PRIMARY";
        case Role::Backup:
            return "BACKUP";
    }
    return "UNKNOWN";
}

const char* toString(ReplicationMode m) noexcept {
    switch (m) {
        case ReplicationMode::Synchronous:
            return "SYNC";
        case ReplicationMode::Asynchronous:
            return "ASYNC";
    }
    return "UNKNOWN";
}

const char* toString(SubmitStatus s) noexcept {
    switch (s) {
        case SubmitStatus::Accepted:
            return "ACCEPTED";
        case SubmitStatus::Busy:
            return "BUSY";
        case SubmitStatus::NotPrimary:
            return "NOT_PRIMARY";
        case SubmitStatus::NoBackup:
            return "NO_BACKUP";
    }
    return "UNKNOWN";
}

Replicator::Replicator(NodeId self, Role role, ReplicationMode mode, MatchingEngine& engine,
                       CommandLog& log, Transport& transport)
    : self_(self), role_(role), mode_(mode), engine_(engine), log_(log), transport_(transport) {}

void Replicator::log(const std::string& msg) const {
    if (onLog_) {
        onLog_(msg);
    }
}

void Replicator::setRole(Role role) {
    if (role == role_) {
        return;
    }
    role_ = role;
    ++stats_.roleChanges;
    // Everything this node knew about its backups was learned while
    // someone else was leading, or in an earlier term. A new leader
    // starts by probing each backup rather than assuming any of it still
    // holds: the previous leader's view of who had what is exactly the
    // information a failover invalidates.
    for (auto& [id, state] : backups_) {
        state.probing = true;
        state.lastSent = 0;
        state.lastApplied = 0;
        state.matchSeq = 0;
        state.lastCommitSent = 0;
    }
    log(std::string("role is now ") + toString(role_));
}

void Replicator::onPeerUp(NodeId id) {
    if (role_ != Role::Primary) {
        return;
    }
    // A reconnecting backup may have applied anything from nothing to
    // everything. The primary cannot know, so it probes: an empty
    // AppendEntries whose prevSeq is the primary's head. The backup
    // either agrees, or reports where it actually got to.
    BackupState& backup = backups_[id];
    backup.id = id;
    backup.probing = true;
    backup.lastSent = 0;
    sendAppend(backup, true);
}

void Replicator::onPeerDown(NodeId id) {
    backups_.erase(id);
}

std::vector<BackupState> Replicator::backups() const {
    std::vector<BackupState> out;
    out.reserve(backups_.size());
    for (const auto& [id, state] : backups_) {
        out.push_back(state);
    }
    return out;
}

Sequence Replicator::commitSeq() const noexcept {
    // Phase 3 defined this as the MINIMUM across all backups: an entry
    // was committed once every backup held it. Simple, and it means one
    // slow or dead backup stalls the whole cluster, which is the
    // opposite of fault tolerance.
    //
    // Phase 5 replaces it with a majority, computed in
    // advanceCommitIndex(). The cluster now makes progress while a
    // minority is down, which is the entire point.
    return commitIndex_;
}

SubmitResult Replicator::submit(const Command& cmd) {
    if (role_ != Role::Primary) {
        return SubmitResult{SubmitStatus::NotPrimary, 0};
    }

    if (mode_ == ReplicationMode::Synchronous) {
        if (backups_.empty()) {
            // Synchronous means a command is only applied once a backup
            // holds it. With no backup, applying anyway would silently
            // downgrade to asynchronous and quietly lose the guarantee
            // the mode exists to provide.
            ++stats_.submitRejectedBusy;
            return SubmitResult{SubmitStatus::NoBackup, 0};
        }
        // The in-flight window bounds how far ahead of the acknowledged
        // point the log may run. Without it, backpressure would have
        // nowhere to appear and the log would grow without bound.
        if (log_.lastSeq() - commitSeq() >= syncWindow_) {
            ++stats_.submitRejectedBusy;
            return SubmitResult{SubmitStatus::Busy, 0};
        }
    }

    // Refuse before appending if any backup is saturated, so the log
    // never runs further ahead than the transport can carry.
    for (const auto& [id, state] : backups_) {
        if (transport_.isSaturated(id)) {
            ++stats_.submitRejectedBusy;
            return SubmitResult{SubmitStatus::Busy, 0};
        }
    }

    const Sequence seq = log_.append(cmd, term_);
    ++stats_.submitted;

    // The leader no longer applies on submit. It applies when the entry
    // is committed, which under Asynchronous is as soon as a majority
    // has it and under Synchronous is the same rule with a tighter
    // window. Applying before commit is what would lose data on a leader
    // crash, because a client could observe a trade that never made it
    // to a majority.
    if (clusterSize_ == 1) {
        // A single-node cluster is its own majority.
        advanceCommitIndex();
    }

    for (auto& [id, state] : backups_) {
        if (!state.probing) {
            sendAppend(state, false);
        }
    }
    return SubmitResult{SubmitStatus::Accepted, seq};
}

void Replicator::sendAppend(BackupState& backup, bool probe) {
    net::AppendEntries msg;
    msg.term = term_;
    msg.leaderId = self_;
    msg.commitSeq = commitSeq();

    if (probe) {
        msg.prevSeq = log_.lastSeq();
        msg.prevTerm = log_.termAt(log_.lastSeq()).value_or(0);
        ++stats_.probesSent;
    } else {
        const Sequence from = std::max(backup.matchSeq, backup.lastSent) + 1;
        if (from > log_.lastSeq()) {
            // No new entries. But if the commit point has moved since we
            // last told this backup, it needs to know, or it will never
            // apply the entries it already holds.
            if (commitIndex_ > backup.lastCommitSent) {
                msg.prevSeq = log_.lastSeq();
                msg.prevTerm = log_.termAt(log_.lastSeq()).value_or(0);
                const auto result = transport_.send(backup.id, Message{std::move(msg)});
                if (result == SendResult::Ok) {
                    backup.lastCommitSent = commitIndex_;
                }
            }
            return;
        }
        if (!log_.canServeFrom(from)) {
            // The entries this backup needs have been truncated away.
            // Phase 3 has no snapshot mechanism, so say so plainly
            // rather than sending a gap and corrupting it.
            log("backup " + std::to_string(backup.id) + " needs seq " + std::to_string(from) +
                " which has been truncated; snapshot required (not implemented)");
            return;
        }
        auto entries = log_.range(from, net::kMaxEntriesPerAppend);
        if (entries.empty()) {
            return;
        }
        msg.prevSeq = from - 1;
        const auto prevTerm = log_.termAt(from - 1);
        msg.prevTerm = prevTerm.value_or(0);
        msg.entries.reserve(entries.size());
        for (const LogEntry& e : entries) {
            msg.entries.push_back(net::LogRecord{e.seq, e.term, e.command});
        }
        if (entries.size() > 1) {
            ++stats_.catchUpBatches;
        }
    }

    // Capture what is needed for bookkeeping BEFORE the move: moving
    // `msg` into the Message leaves its entries vector empty, so reading
    // it afterwards silently reports an empty batch. That exact mistake
    // left lastSent permanently at zero, so every submit re-sent the
    // whole outstanding range from the beginning. It was invisible to
    // the correctness tests, because re-sending is harmless to state,
    // and showed up only as 883,003 messages and 91 MB of traffic to
    // replicate 1,000 commands. See docs/failure-modes.md.
    const std::size_t batchSize = msg.entries.size();
    const Sequence lastInBatch = batchSize == 0 ? 0 : msg.entries.back().seq;

    const SendResult result = transport_.send(backup.id, Message{std::move(msg)});
    if (result != SendResult::Ok) {
        // Saturated or disconnected. Do not advance lastSent, so tick()
        // retries the same range rather than skipping it.
        return;
    }
    if (!probe && batchSize > 0) {
        backup.lastSent = lastInBatch;
        backup.lastCommitSent = commitIndex_;
        stats_.entriesSent += batchSize;
    }
}

void Replicator::handleAppendEntries(NodeId from, const net::AppendEntries& msg) {
    // A leader from a previous term has been superseded. Ignoring its
    // entries is what stops a stale leader corrupting a follower that
    // has already moved on. The election layer owns the term itself;
    // the replicator only has to refuse to act on an old one.
    if (msg.term < term_) {
        ++stats_.staleTermRejections;
        net::AppendResponse reject;
        reject.term = term_;
        reject.nodeId = self_;
        reject.ok = false;
        reject.lastApplied = lastApplied_;
        reject.lastLogSeq = log_.lastSeq();
        reject.stateChecksum = engine_.stateChecksum();
        transport_.send(from, Message{reject});
        return;
    }

    net::AppendResponse response;
    response.term = term_;
    response.nodeId = self_;

    // LOG MATCHING.
    //
    // Phase 3 only checked the sequence number, which was enough while
    // one node had ever written the log. Once leaders can change, two
    // leaders in different terms can each have written a DIFFERENT entry
    // at the same sequence, so a follower must check that its entry at
    // prevSeq has the same TERM the leader expects. Matching on the
    // number alone would splice two different histories together and
    // produce a book that never existed on any node.
    const auto localPrevTerm = log_.termAt(msg.prevSeq);
    if (!localPrevTerm.has_value() || localPrevTerm.value() != msg.prevTerm) {
        if (!localPrevTerm.has_value()) {
            ++stats_.gapsDetected;
        } else {
            ++stats_.logMatchRejections;
        }
        response.ok = false;
        response.lastApplied = lastApplied_;
        response.lastLogSeq = log_.lastSeq();
        response.stateChecksum = engine_.stateChecksum();
        transport_.send(from, Message{response});
        return;
    }

    sententia::EventList sink;
    for (const net::LogRecord& record : msg.entries) {
        const auto existing = log_.termAt(record.seq);
        if (existing.has_value() && record.seq <= log_.lastSeq()) {
            if (existing.value() == record.term) {
                continue;  // already have exactly this entry
            }
            // CONFLICT. We hold a different entry at this sequence,
            // written by a leader that has since been superseded. Ours
            // is wrong, so the divergent tail goes.
            //
            // This is only ever safe because of the commit rule: an
            // entry that was committed is held by a majority, and the
            // election restriction means no candidate missing it can
            // win. So anything truncated here was never committed, and
            // no client was ever told it succeeded.
            if (record.seq <= commitIndex_) {
                // Truncating a committed entry would mean the safety
                // argument had failed somewhere. Refuse rather than
                // corrupt, and make it loud.
                log("REFUSING to truncate committed entry at seq " + std::to_string(record.seq) +
                    "; this should be impossible");
                response.ok = false;
                response.lastApplied = lastApplied_;
                response.stateChecksum = engine_.stateChecksum();
                transport_.send(from, Message{response});
                return;
            }
            log_.truncateFrom(record.seq);
            ++stats_.conflictingEntriesTruncated;
            // The engine has already applied the entries being thrown
            // away, so it has to be rebuilt from the surviving prefix.
            rebuildFromLog();
        }
        if (!log_.appendAt(record.seq, record.term, record.command)) {
            ++stats_.gapsDetected;
            break;
        }
        ++stats_.entriesReceived;
    }

    // A follower applies only up to the leader's commit point, never to
    // the end of its own log. Entries past the commit point may still be
    // truncated, and applying them early would let a client observe a
    // trade that later un-happens.
    if (msg.commitSeq > commitIndex_) {
        commitIndex_ = std::min(msg.commitSeq, log_.lastSeq());
        ++stats_.commitAdvances;
    }
    applyCommitted();

    response.ok = true;
    response.lastApplied = lastApplied_;
    response.lastLogSeq = log_.lastSeq();
    response.stateChecksum = engine_.stateChecksum();
    transport_.send(from, Message{response});
}

void Replicator::applyCommitted() {
    sententia::EventList sink;
    while (lastApplied_ < commitIndex_) {
        const LogEntry* entry = log_.at(lastApplied_ + 1);
        if (entry == nullptr) {
            break;
        }
        sink.clear();
        engine_.apply(entry->command, sink);
        lastApplied_ = entry->seq;
        ++stats_.applied;
    }
}

void Replicator::rebuildFromLog() {
    // Replay the retained log from the snapshot boundary. Deterministic,
    // so this reproduces exactly the state the surviving prefix implies.
    sententia::EngineSnapshot base = engine_.snapshot();
    (void)base;
    engine_ = sententia::MatchingEngine(engine_.instrument());
    if (snapshotBase_.has_value()) {
        engine_.restore(snapshotBase_.value());
    }
    lastApplied_ = log_.snapshotSeq();
    sententia::EventList sink;
    for (Sequence seq = log_.snapshotSeq() + 1; seq <= log_.lastSeq(); ++seq) {
        const LogEntry* entry = log_.at(seq);
        if (entry == nullptr) {
            break;
        }
        sink.clear();
        engine_.apply(entry->command, sink);
        lastApplied_ = seq;
    }
    if (commitIndex_ > lastApplied_) {
        commitIndex_ = lastApplied_;
    }
}

void Replicator::advanceCommitIndex() {
    if (role_ != Role::Primary) {
        return;
    }
    // MAJORITY COMMIT.
    //
    // Collect every node's match point, including the leader's own log
    // head, sort descending, and take the value at index majority-1.
    // That is the highest sequence a majority holds.
    //
    // Phase 3 used the MINIMUM across all backups, which is stricter and
    // simpler but means one slow or dead backup stalls the cluster. A
    // majority keeps making progress while a minority is down, which is
    // the entire point of tolerating failure.
    std::vector<Sequence> matches;
    matches.reserve(backups_.size() + 1);
    matches.push_back(log_.lastSeq());
    for (const auto& [id, state] : backups_) {
        matches.push_back(state.matchSeq);
    }
    // A cluster larger than the peers we can currently see still needs a
    // majority of the CONFIGURED size, or a partitioned minority could
    // commit on its own.
    while (matches.size() < clusterSize_) {
        matches.push_back(0);
    }
    std::sort(matches.begin(), matches.end(), std::greater<Sequence>());
    const Sequence candidate = matches[majority() - 1];

    if (candidate <= commitIndex_) {
        return;
    }

    // THE FIGURE 8 RULE, and it is the subtlest thing in the project.
    //
    // A leader may NOT commit an entry from a previous term just because
    // a majority now holds it. Raft's paper has a five-node scenario
    // where doing so lets a committed entry be overwritten later.
    //
    // The reason: a majority holding an old entry does not mean that
    // entry is safe, because a future leader could still be elected
    // without it. Only once the current leader has committed an entry
    // from its OWN term does the election restriction guarantee every
    // future leader carries everything up to that point. Committing an
    // entry from the current term carries the earlier ones with it.
    const auto candidateTerm = log_.termAt(candidate);
    if (!candidateTerm.has_value() || candidateTerm.value() != term_) {
        return;
    }

    commitIndex_ = candidate;
    ++stats_.commitAdvances;
    applyCommitted();
}

void Replicator::handleAppendResponse(NodeId from, const net::AppendResponse& msg) {
    // A follower in a later term means this node is a stale leader. The
    // election layer will see the same term on its own path and step
    // this node down; there is nothing useful to do with the response.
    if (msg.term > term_) {
        ++stats_.staleTermRejections;
        return;
    }

    auto it = backups_.find(from);
    if (it == backups_.end()) {
        BackupState fresh;
        fresh.id = from;
        it = backups_.emplace(from, fresh).first;
    }
    BackupState& backup = it->second;

    backup.lastApplied = msg.lastApplied;
    // The match point is the follower's LOG head, not what it has
    // applied. See the comment on AppendResponse::lastLogSeq.
    if (msg.ok) {
        backup.matchSeq = msg.lastLogSeq;
    }
    backup.lastChecksum = msg.stateChecksum;
    backup.probing = false;
    // Never let lastSent sit ahead of what the backup admits to holding
    // after a negative response, or the missing range is never resent.
    if (!msg.ok || backup.lastSent < msg.lastApplied) {
        backup.lastSent = msg.lastApplied;
    }

    if (!msg.ok) {
        log("backup " + std::to_string(from) + " reports gap, resending from " +
            std::to_string(msg.lastApplied + 1));
    }

    // Recompute the commit point from a majority, then apply up to it.
    // Under both modes the leader applies only committed entries now,
    // which is what makes a leader crash safe: nothing a client saw can
    // be lost, and nothing unlogged was ever shown.
    advanceCommitIndex();

    // Divergence check. Two nodes that have applied the same sequence
    // must agree on state; this compares a single integer rather than
    // a book. Only meaningful when the primary has actually applied to
    // the same point, which under async it usually has not.
    if (backup.lastApplied == lastApplied_ && backup.lastChecksum != engine_.stateChecksum()) {
        ++stats_.checksumMismatches;
        log("DIVERGENCE: backup " + std::to_string(from) + " checksum " +
            std::to_string(backup.lastChecksum) + " != primary " +
            std::to_string(engine_.stateChecksum()) + " at seq " + std::to_string(lastApplied_));
    }

    // More to send, or a newly advanced commit point to advertise.
    if (backup.lastSent < log_.lastSeq() || commitIndex_ > backup.lastCommitSent) {
        sendAppend(backup, false);
    }
}

void Replicator::onMessage(NodeId from, const Message& msg) {
    if (const auto* append = std::get_if<net::AppendEntries>(&msg)) {
        handleAppendEntries(from, *append);
        return;
    }
    if (const auto* response = std::get_if<net::AppendResponse>(&msg)) {
        handleAppendResponse(from, *response);
        return;
    }
}

void Replicator::tick() {
    if (role_ != Role::Primary) {
        return;
    }
    for (auto& [id, state] : backups_) {
        if (state.probing) {
            continue;  // waiting on the probe response
        }
        // Retry anything the transport refused earlier, and drive
        // catch-up for a backup that is behind.
        if (transport_.isSaturated(id)) {
            continue;
        }
        // Either there are new entries to send, or the commit point has
        // moved and the backup has not been told.
        if (state.lastSent < log_.lastSeq() || commitIndex_ > state.lastCommitSent) {
            sendAppend(state, false);
        }
    }
}

void Replicator::compactLog() {
    if (role_ != Role::Primary || backups_.empty()) {
        return;
    }
    // Only entries every backup has acknowledged, and that the primary
    // itself has applied, may go. Anything else might still be needed
    // for a catch-up.
    const Sequence safe = std::min(commitSeq(), lastApplied_);
    if (safe == 0) {
        return;
    }
    const std::size_t before = log_.size();
    log_.truncateThrough(safe);
    if (log_.size() != before) {
        ++stats_.logTruncations;
    }
}

}  // namespace sententia::replication
