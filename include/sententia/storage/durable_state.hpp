// Sententia - the durability layer, tying the pieces together.
//
// Three files per node:
//
//   <dir>/state          term and vote, written before any vote is cast
//   <dir>/log.wal        the command log, written before any commit
//   <dir>/snapshot       engine state as of some sequence number
//
// RECOVERY, which is the whole point:
//
//   1. Load the snapshot, if any. That gives an engine as of some
//      sequence S, without replaying anything before it.
//   2. Replay the WAL entries after S.
//   3. The node is now exactly where it was, because the engine is
//      deterministic. Same commands, same order, same state.
//
// Step 3 is Phase 1 paying off for the fourth time. Recovery is not a
// special code path that reconstructs state some other way; it is the
// ordinary apply loop fed from disk instead of from a socket.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "sententia/engine.hpp"
#include "sententia/replication/command_log.hpp"
#include "sententia/storage/snapshot.hpp"
#include "sententia/storage/stable_store.hpp"
#include "sententia/storage/wal.hpp"

namespace sententia::storage {

using sententia::replication::CommandLog;
using sententia::replication::LogEntry;

struct RecoveryReport {
    bool hadSnapshot{false};
    std::uint64_t snapshotSeq{0};
    std::uint64_t entriesReplayed{0};
    std::uint64_t lastLogSeq{0};
    std::uint64_t currentTerm{0};
    NodeId votedFor{0};
    bool tornTailDiscarded{false};
    std::uint64_t tornTailBytes{0};
};

class DurableState {
public:
    // Opens (or creates) the three files under `dir`.
    bool open(const std::string& dir, SyncPolicy policy, std::string& error);

    // Rebuilds the engine and the log from disk. Safe to call on a fresh
    // directory: it simply reports nothing to recover.
    bool recover(MatchingEngine& engine, CommandLog& log, RecoveryReport& report,
                 std::string& error);

    // Writes an entry to the WAL. The caller must not treat the entry as
    // durable, and must not count it towards a commit, until this
    // returns true. That ordering is the write-ahead rule.
    bool appendEntry(const LogEntry& entry, std::string& error);

    // Persists term and vote. Must complete before the node acts on
    // either, or a crash can make it vote twice in one term.
    bool saveVote(std::uint64_t term, NodeId votedFor, std::string& error);

    // Snapshots the engine and truncates the WAL, so recovery no longer
    // has to replay everything before `lastIncludedSeq`.
    bool takeSnapshot(const MatchingEngine& engine, std::uint64_t lastIncludedSeq,
                      std::uint64_t lastIncludedTerm, std::string& error);

    // Replaces local state wholesale with a snapshot received from a
    // leader, for a node too far behind to catch up by replay.
    bool installSnapshot(const SnapshotRecord& snap, MatchingEngine& engine, CommandLog& log,
                         std::string& error);

    bool sync(std::string& error) { return wal_.sync(error); }

    const StableState& stable() const noexcept { return stable_.state(); }
    const WalStats& walStats() const noexcept { return wal_.stats(); }
    const StableStoreStats& stableStats() const noexcept { return stable_.stats(); }
    std::uint64_t walBytes() const noexcept { return wal_.sizeBytes(); }
    const std::optional<SnapshotRecord>& snapshot() const noexcept { return snapshot_; }
    const std::string& dir() const noexcept { return dir_; }

private:
    std::string dir_;
    WriteAheadLog wal_;
    StableStore stable_;
    std::optional<SnapshotRecord> snapshot_;
};

// Serialisation for one log entry, shared by the WAL and by
// InstallSnapshot. Same explicit little-endian discipline as the wire.
void encodeLogEntry(const LogEntry& entry, net::Buffer& out);
std::optional<LogEntry> decodeLogEntry(const net::Byte* data, std::size_t size);

}  // namespace sententia::storage
