// Sententia - durable term and vote.
//
// This closes the correctness hole Phase 4 shipped with, and it is worth
// being precise about why it was a hole rather than a nicety.
//
// Raft's no-split-brain argument rests on "each node votes at most once
// per term". If a node votes for A in term 5, crashes, restarts with no
// memory of that vote, and then votes for B in term 5, two candidates
// can each collect a majority. Two leaders in one term. The arithmetic
// that made split-brain impossible assumed the vote was remembered, and
// memory does not survive a crash.
//
// So the term and the vote must be on disk BEFORE a node replies to a
// RequestVote. Same write-ahead ordering as the command log: make the
// promise durable, then make it.
//
// The file is tiny, so it is written whole every time, to a temporary
// file which is then fsynced and atomically renamed over the original.
// Rename within a directory is atomic on POSIX, so a crash leaves either
// the complete old file or the complete new one, never a half-written
// mixture. Writing in place would leave exactly the torn state this is
// trying to avoid.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "sententia/net/message.hpp"

namespace sententia::storage {

using sententia::net::NodeId;

struct StableState {
    std::uint64_t currentTerm{0};
    // 0 means "no vote cast in this term". Node ids start at 1.
    NodeId votedFor{0};

    friend bool operator==(const StableState&, const StableState&) = default;
};

struct StableStoreStats {
    std::uint64_t writes{0};
    std::uint64_t fsyncs{0};
    bool loadedExisting{false};
    bool recoveredFromCorruption{false};
};

class StableStore {
public:
    // Loads existing state if the file is present and intact. A missing
    // file is a fresh node, not an error. A corrupt file is treated as
    // missing and reported through stats, because the alternative is
    // refusing to start on a node that can safely rebuild by voting
    // conservatively.
    bool open(const std::string& path, std::string& error);

    // Persists and flushes before returning. The caller must not act on
    // the new term or vote until this returns true.
    bool save(const StableState& state, std::string& error);

    const StableState& state() const noexcept { return state_; }
    const StableStoreStats& stats() const noexcept { return stats_; }
    const std::string& path() const noexcept { return path_; }

private:
    std::string path_;
    StableState state_;
    StableStoreStats stats_;
};

}  // namespace sententia::storage
