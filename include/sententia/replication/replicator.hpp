// Sententia - state-machine replication, primary to backup.
//
// The idea, and it is the whole idea:
//
//   Because the engine is a deterministic state machine (Phase 1), two
//   nodes that apply the same commands in the same order reach the same
//   state. So replication does not need to ship state at all. It ships
//   the commands, and each node computes the state itself.
//
// The message is a few dozen bytes regardless of how deep the book gets.
// Shipping state would cost megabytes per order and would get more
// expensive exactly when the venue is busiest.
//
// This is state-machine replication, the idea underneath Raft, Paxos,
// and every database that replicates by shipping its write-ahead log.
// Its one precondition is determinism, which is why Phase 1 came first
// and why its determinism test is the gate on everything here.
//
// ORDER IS THE OTHER HALF. Phase 1's sensitivity test showed that
// swapping two adjacent commands changes the output. So the log's
// *order* matters as much as its contents, and the primary is what
// imposes it. That is also precisely why Phase 4 needs an election: with
// no agreed primary there is no agreed order.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "sententia/engine.hpp"
#include "sententia/net/transport.hpp"
#include "sententia/replication/command_log.hpp"

namespace sententia::replication {

using sententia::MatchingEngine;
using sententia::net::Message;
using sententia::net::NodeId;
using sententia::net::Transport;

enum class Role {
    Primary,
    Backup,
};

// The consistency decision, made concrete. This is the CAP tradeoff in
// two enum values, and the reason both are implemented rather than one
// is that the difference is worth being able to measure.
enum class ReplicationMode {
    // The primary applies a command only once a backup has acknowledged
    // holding it. A client told "done" means both nodes have it, so a
    // primary crash loses nothing. Costs a network round trip per
    // command, and the primary can go no faster than its slowest backup.
    Synchronous,

    // The primary applies immediately and replicates in the background.
    // Fast, and the backup may lag. A primary crash can lose whatever
    // had not reached the backup yet.
    Asynchronous,
};

const char* toString(Role r) noexcept;
const char* toString(ReplicationMode m) noexcept;

enum class SubmitStatus {
    // Accepted into the log. Under Synchronous it is not applied yet;
    // it becomes applied when a backup acknowledges it.
    Accepted,
    // Synchronous mode with the in-flight window full, or a backup's
    // send queue saturated. The caller must retry. This is backpressure
    // surfacing all the way to the client, which is the honest place
    // for it to appear.
    Busy,
    // Not the primary. Backups do not accept client commands.
    NotPrimary,
    // No backup is connected and the mode requires one.
    NoBackup,
};

const char* toString(SubmitStatus s) noexcept;

struct SubmitResult {
    SubmitStatus status{SubmitStatus::Busy};
    Sequence seq{};

    bool ok() const noexcept { return status == SubmitStatus::Accepted; }
};

struct ReplicationStats {
    std::uint64_t submitted{0};
    std::uint64_t submitRejectedBusy{0};
    std::uint64_t applied{0};
    std::uint64_t entriesSent{0};
    std::uint64_t entriesReceived{0};
    std::uint64_t gapsDetected{0};
    std::uint64_t catchUpBatches{0};
    std::uint64_t probesSent{0};
    std::uint64_t checksumMismatches{0};
    std::uint64_t logTruncations{0};
    std::uint64_t roleChanges{0};
    std::uint64_t staleTermRejections{0};
    std::uint64_t logMatchRejections{0};
    std::uint64_t conflictingEntriesTruncated{0};
    std::uint64_t snapshotsInstalled{0};
    std::uint64_t snapshotsTaken{0};
    std::uint64_t recoveredFromLog{0};
    std::uint64_t commitAdvances{0};
};

// What the primary knows about one backup.
struct BackupState {
    NodeId id{};
    // Raft's matchIndex: the highest entry known to be replicated here.
    // The commit point is derived from a MAJORITY of these, not from all
    // of them, which is what lets the cluster make progress with a slow
    // or dead minority.
    Sequence matchSeq{0};
    // The commit point this backup has already been told about. Without
    // tracking it, the last entry of a burst never becomes applied on
    // the follower: the leader commits it only after hearing the
    // follower holds it, and by then it has nothing left to send, so the
    // new commit point is never advertised. The follower sits one entry
    // behind forever. Raft avoids this with periodic heartbeats; this
    // sends an empty append whenever the commit point moves.
    Sequence lastCommitSent{0};
    // Highest sequence this backup reports having applied.
    Sequence lastApplied{0};
    // Highest sequence sent to it, so a catch-up is not re-sent every
    // tick while the first batch is still in flight.
    Sequence lastSent{0};
    std::uint64_t lastChecksum{0};
    bool probing{true};
};

class Replicator {
public:
    Replicator(NodeId self, Role role, ReplicationMode mode, MatchingEngine& engine,
               CommandLog& log, Transport& transport);

    // Primary only. Assigns a sequence number, appends to the log, and
    // replicates. Under Asynchronous the command is applied before this
    // returns; under Synchronous it is applied when acknowledged.
    SubmitResult submit(const Command& cmd);

    // Feed every message the transport delivers through here.
    void onMessage(NodeId from, const Message& msg);

    // Call when a peer connects. The primary probes it to find out where
    // it got to, because the primary cannot know that on its own.
    void onPeerUp(NodeId id);
    void onPeerDown(NodeId id);

    // Drives catch-up and retries. Call once per poll cycle.
    void tick();

    // Called by the election when this node becomes or stops being
    // leader. Taking over resets what the replicator believed about
    // every backup, because that knowledge belonged to the previous
    // leader and may be wrong now.
    void setRole(Role role);

    // The current election term, stamped onto outgoing AppendEntries so
    // a follower can tell a live leader from a stale one.
    void setTerm(std::uint64_t term) noexcept { term_ = term; }
    std::uint64_t term() const noexcept { return term_; }

    Role role() const noexcept { return role_; }
    ReplicationMode mode() const noexcept { return mode_; }

    // Highest sequence applied to the local engine.
    Sequence lastApplied() const noexcept { return lastApplied_; }
    // Highest sequence every backup has acknowledged. Under Synchronous
    // this is what "committed" means.
    Sequence commitSeq() const noexcept;
    std::uint64_t stateChecksum() const noexcept { return engine_.stateChecksum(); }

    const ReplicationStats& stats() const noexcept { return stats_; }
    std::vector<BackupState> backups() const;
    bool hasBackup() const noexcept { return !backups_.empty(); }

    // Maximum unacknowledged entries under Synchronous. One is strict
    // lockstep; higher values pipeline, trading a little crash-loss
    // exposure for throughput.
    void setSyncWindow(std::size_t n) noexcept { syncWindow_ = n; }
    std::size_t syncWindow() const noexcept { return syncWindow_; }

    // Drop log entries every backup has acknowledged. Without this the
    // log grows for the life of the process.
    void compactLog();

    // Number of nodes in the cluster, including this one. Needed because
    // the commit point is a majority, and a majority of what has to be
    // known. Defaults to 2 (one leader, one backup), which is what
    // Phase 3 effectively assumed.
    void setClusterSize(std::size_t n) noexcept { clusterSize_ = n < 1 ? 1 : n; }
    std::size_t clusterSize() const noexcept { return clusterSize_; }
    std::size_t majority() const noexcept { return clusterSize_ / 2 + 1; }

    // Applies everything up to the commit point. A follower calls this
    // when the leader tells it the commit point moved.
    void applyCommitted();
    Sequence commitIndex() const noexcept { return commitIndex_; }

    void onLog(std::function<void(const std::string&)> h) { onLog_ = std::move(h); }

private:
    // Recomputes the commit point from a majority of matchSeq values.
    void advanceCommitIndex();
    void rebuildFromLog();
    void sendAppend(BackupState& backup, bool probe);
    void handleAppendEntries(NodeId from, const net::AppendEntries& msg);
    void handleAppendResponse(NodeId from, const net::AppendResponse& msg);
    void log(const std::string& msg) const;

    NodeId self_{};
    Role role_{Role::Primary};
    ReplicationMode mode_{ReplicationMode::Synchronous};
    MatchingEngine& engine_;
    CommandLog& log_;
    Transport& transport_;

    Sequence lastApplied_{0};
    Sequence commitIndex_{0};
    std::uint64_t term_{0};
    std::size_t clusterSize_{2};
    // The snapshot this node's log continues from, if any.
    std::optional<sententia::EngineSnapshot> snapshotBase_;
    std::size_t syncWindow_{1};
    std::map<NodeId, BackupState> backups_;
    ReplicationStats stats_;
    std::function<void(const std::string&)> onLog_;
};

}  // namespace sententia::replication
