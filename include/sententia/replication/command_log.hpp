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
    // The election term the leader was serving when it created this
    // entry. Phase 3 did not need this, because only one node ever
    // wrote the log. Once leaders can change, two nodes can each have
    // written a DIFFERENT entry at the same sequence number, and the
    // term is what tells them apart. Without it a follower cannot know
    // whether its entry at seq 7 is the same entry the leader has.
    std::uint64_t term{};
    Command command{};

    friend bool operator==(const LogEntry&, const LogEntry&) = default;
};

class CommandLog {
public:
    CommandLog() = default;

    // Appends with the next sequence number, stamped with `term`.
    Sequence append(const Command& cmd, std::uint64_t term = 0);

    // Appends at an explicit sequence number. Follower side, replaying
    // what the leader sent. Refuses anything but exactly lastSeq() + 1,
    // because accepting a gap would silently break the determinism
    // contract the whole design rests on.
    bool appendAt(Sequence seq, std::uint64_t term, const Command& cmd);

    // The term of the entry at `seq`, or nullopt if it is not held.
    // Returns the snapshot's term for the sequence the log starts after,
    // so log matching still works across a compacted prefix.
    std::optional<std::uint64_t> termAt(Sequence seq) const noexcept;

    // Discards everything from `seq` onward. This is the operation Phase
    // 3 never needed: a follower whose log conflicts with the leader's
    // must throw its divergent tail away. Uncommitted entries only; the
    // commit rule is what guarantees nothing committed is ever here.
    void truncateFrom(Sequence seq);

    // Records the snapshot boundary so termAt and canServeFrom stay
    // correct after the prefix has been compacted away.
    void setSnapshotBoundary(Sequence seq, std::uint64_t term) noexcept;
    Sequence snapshotSeq() const noexcept { return snapshotSeq_; }
    std::uint64_t snapshotTerm() const noexcept { return snapshotTerm_; }

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
    Sequence snapshotSeq_{0};
    std::uint64_t snapshotTerm_{0};
    // Highest sequence ever truncated away, so canServeFrom can tell
    // "not yet written" from "written but discarded".
    Sequence truncatedThrough_{0};
};

}  // namespace sententia::replication
