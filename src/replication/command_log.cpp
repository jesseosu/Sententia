#include "sententia/replication/command_log.hpp"

namespace sententia::replication {

Sequence CommandLog::append(const Command& cmd, std::uint64_t term) {
    const Sequence seq = lastSeq_ + 1;
    entries_.push_back(LogEntry{seq, term, cmd});
    lastSeq_ = seq;
    return seq;
}

bool CommandLog::appendAt(Sequence seq, std::uint64_t term, const Command& cmd) {
    if (seq != lastSeq_ + 1) {
        return false;
    }
    entries_.push_back(LogEntry{seq, term, cmd});
    lastSeq_ = seq;
    return true;
}

std::optional<std::uint64_t> CommandLog::termAt(Sequence seq) const noexcept {
    // Sequence 0 is the empty log, by convention term 0.
    if (seq == 0) {
        return std::uint64_t{0};
    }
    // The entry immediately before the retained window lives in the
    // snapshot, and its term was recorded there. Without this, log
    // matching would fail forever after a compaction.
    if (seq == snapshotSeq_ && snapshotSeq_ != 0) {
        return snapshotTerm_;
    }
    const LogEntry* entry = at(seq);
    if (entry == nullptr) {
        return std::nullopt;
    }
    return entry->term;
}

void CommandLog::truncateFrom(Sequence seq) {
    while (!entries_.empty() && entries_.back().seq >= seq) {
        entries_.pop_back();
    }
    lastSeq_ = entries_.empty()
                   ? (snapshotSeq_ > truncatedThrough_ ? snapshotSeq_ : truncatedThrough_)
                   : entries_.back().seq;
}

void CommandLog::setSnapshotBoundary(Sequence seq, std::uint64_t term) noexcept {
    snapshotSeq_ = seq;
    snapshotTerm_ = term;
    if (lastSeq_ < seq) {
        lastSeq_ = seq;
    }
    if (truncatedThrough_ < seq) {
        truncatedThrough_ = seq;
    }
}

const LogEntry* CommandLog::at(Sequence seq) const noexcept {
    if (entries_.empty() || seq < firstSeq() || seq > lastSeq_) {
        return nullptr;
    }
    const auto offset = static_cast<std::size_t>(seq - firstSeq());
    return &entries_[offset];
}

std::vector<LogEntry> CommandLog::range(Sequence from, std::size_t maxCount) const {
    std::vector<LogEntry> out;
    if (entries_.empty() || from > lastSeq_ || maxCount == 0) {
        return out;
    }
    if (from < firstSeq()) {
        // The caller wants entries that have been truncated away. Return
        // nothing rather than silently starting later, which would hand
        // back a gap disguised as a valid range.
        return out;
    }
    const auto start = static_cast<std::size_t>(from - firstSeq());
    const std::size_t available = entries_.size() - start;
    const std::size_t count = available < maxCount ? available : maxCount;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(entries_[start + i]);
    }
    return out;
}

Sequence CommandLog::firstSeq() const noexcept {
    return entries_.empty() ? truncatedThrough_ + 1 : entries_.front().seq;
}

bool CommandLog::canServeFrom(Sequence from) const noexcept {
    // Asking for the next sequence that does not exist yet is fine: the
    // backup is simply current. Asking for something discarded is not.
    if (from == lastSeq_ + 1) {
        return true;
    }
    return from >= firstSeq() && from <= lastSeq_;
}

void CommandLog::truncateThrough(Sequence seq) {
    while (!entries_.empty() && entries_.front().seq <= seq) {
        truncatedThrough_ = entries_.front().seq;
        entries_.pop_front();
    }
}

void CommandLog::clear() noexcept {
    entries_.clear();
    lastSeq_ = 0;
    truncatedThrough_ = 0;
}

}  // namespace sententia::replication
