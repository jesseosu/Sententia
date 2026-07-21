// Sententia - the replicated command log.
//
// State-machine replication ships *commands*, not state. This is where
// the primary keeps them, and it exists for one reason that only became
// necessary in Phase 3: a backup that falls behind has to be able to ask
// for what it missed, and the primary can only answer if it kept a copy.
//
// The log is the authoritative ordering. The engine's own command
// counter is an internal detail; the sequence numbers here are what two
// nodes agree on.
//
// In memory only, for now. A durable log is Phase 5's problem, and
// designing the on-disk format before recovery exists would be guessing.
// The interface is shaped so that adding durability later does not
// change how callers use it.
#pragma once

#include <cstddef>
#include <deque>
#include <optional>
#include <vector>

#include "sententia/command.hpp"
#include "sententia/types.hpp"

namespace sententia::replication {

using sententia::Command;
using sententia::Sequence;

struct LogEntry {
    // Primary-assigned, monotonic, gapless, starting at 1. Distinct from
    // the engine's internal command counter: this one is agreed between
    // nodes, and it is the thing the backup checks for gaps.
    Sequence seq{};
    Command command{};

    friend bool operator==(const LogEntry&, const LogEntry&) = default;
};

class CommandLog {
public:
    CommandLog() = default;

    // Appends with the next sequence number. Primary side.
    Sequence append(const Command& cmd);

    // Appends at an explicit sequence number. Backup side, replaying
    // what the primary sent. Refuses anything but exactly lastSeq() + 1,
    // because accepting a gap would silently break the determinism
    // contract the whole design rests on.
    bool appendAt(Sequence seq, const Command& cmd);

    const LogEntry* at(Sequence seq) const noexcept;

    // Entries [from, from + maxCount), clamped to what exists. Empty if
    // `from` has already been truncated away or is past the end.
    std::vector<LogEntry> range(Sequence from, std::size_t maxCount) const;

    // Oldest retained sequence. Greater than 1 once truncation has run.
    Sequence firstSeq() const noexcept;
    // Highest appended sequence. Zero when empty, which is why sequence
    // numbers start at 1: zero means "nothing yet".
    Sequence lastSeq() const noexcept { return lastSeq_; }

    std::size_t size() const noexcept { return entries_.size(); }
    bool empty() const noexcept { return entries_.empty(); }

    // Whether the log can still serve a catch-up starting at `from`.
    // False means the backup has fallen so far behind that the entries
    // it needs are gone, and it needs a snapshot instead of a replay.
    // Phase 3 does not implement snapshots; it reports the condition
    // honestly rather than pretending it cannot happen.
    bool canServeFrom(Sequence from) const noexcept;

    // Drops entries at or below `seq`. The primary calls this once every
    // backup has acknowledged them, which is what keeps the log from
    // growing without bound over a long session.
    void truncateThrough(Sequence seq);

    void clear() noexcept;

private:
    std::deque<LogEntry> entries_;
    Sequence lastSeq_{0};
    // Highest sequence ever truncated away, so canServeFrom can tell
    // "not yet written" from "written but discarded".
    Sequence truncatedThrough_{0};
};

}  // namespace sententia::replication
