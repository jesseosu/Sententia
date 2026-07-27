#include "sententia/replication/replicator.hpp"

#include <algorithm>

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
    if (backups_.empty()) {
        return 0;
    }
    // Committed means every backup holds it. With one backup this is
    // just its position; the min generalises to more without changing
    // the meaning.
    Sequence lowest = backups_.begin()->second.lastApplied;
    for (const auto& [id, state] : backups_) {
        lowest = std::min(lowest, state.lastApplied);
    }
    return lowest;
}

void Replicator::applyThrough(Sequence seq) {
    sententia::EventList sink;
    while (lastApplied_ < seq) {
        const LogEntry* entry = log_.at(lastApplied_ + 1);
        if (entry == nullptr) {
            // The entry is not available, so stop rather than skipping
            // it. Applying out of order would break determinism in a way
            // no later check could repair.
            break;
        }
        sink.clear();
        engine_.apply(entry->command, sink);
        lastApplied_ = entry->seq;
        ++stats_.applied;
    }
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

    const Sequence seq = log_.append(cmd);
    ++stats_.submitted;

    if (mode_ == ReplicationMode::Asynchronous) {
        // Apply now; the backup catches up on its own schedule.
        applyThrough(seq);
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
    msg.leaderId = self_;
    msg.commitSeq = commitSeq();

    if (probe) {
        msg.prevSeq = log_.lastSeq();
        ++stats_.probesSent;
    } else {
        const Sequence from = std::max(backup.lastApplied, backup.lastSent) + 1;
        if (from > log_.lastSeq()) {
            return;  // backup is current, nothing to send
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
        msg.entries.reserve(entries.size());
        for (const LogEntry& e : entries) {
            msg.entries.push_back(net::LogRecord{e.seq, e.command});
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
        stats_.entriesSent += batchSize;
    }
}

void Replicator::handleAppendEntries(NodeId from, const net::AppendEntries& msg) {
    net::AppendResponse response;
    response.nodeId = self_;

    // A gap. The primary's entries do not follow on from what we have,
    // so applying them would produce a different state than the primary
    // computed. Report our real position and let the primary resend.
    if (msg.prevSeq != lastApplied_) {
        ++stats_.gapsDetected;
        response.ok = false;
        response.lastApplied = lastApplied_;
        response.stateChecksum = engine_.stateChecksum();
        transport_.send(from, Message{response});
        return;
    }

    sententia::EventList sink;
    for (const net::LogRecord& record : msg.entries) {
        if (!log_.appendAt(record.seq, record.command)) {
            // Out of order within the batch itself. Stop and resync.
            ++stats_.gapsDetected;
            break;
        }
        sink.clear();
        engine_.apply(record.command, sink);
        lastApplied_ = record.seq;
        ++stats_.applied;
        ++stats_.entriesReceived;
    }

    response.ok = true;
    response.lastApplied = lastApplied_;
    response.stateChecksum = engine_.stateChecksum();
    transport_.send(from, Message{response});
}

void Replicator::handleAppendResponse(NodeId from, const net::AppendResponse& msg) {
    auto it = backups_.find(from);
    if (it == backups_.end()) {
        BackupState fresh;
        fresh.id = from;
        it = backups_.emplace(from, fresh).first;
    }
    BackupState& backup = it->second;

    backup.lastApplied = msg.lastApplied;
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

    if (mode_ == ReplicationMode::Synchronous) {
        // A command is applied on the primary only once a backup holds
        // it. This is what makes "the client was told done" mean "both
        // nodes have it".
        applyThrough(commitSeq());
    }

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

    // More to send.
    if (backup.lastSent < log_.lastSeq()) {
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
        if (state.lastSent < log_.lastSeq() && !transport_.isSaturated(id)) {
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
