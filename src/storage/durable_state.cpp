#include "sententia/storage/durable_state.hpp"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <cerrno>

#include "sententia/net/wire.hpp"

namespace sententia::storage {
namespace {

using sententia::net::WireReader;
using sententia::net::WireWriter;

std::string join(const std::string& dir, const char* name) {
    if (dir.empty() || dir.back() == '/') {
        return dir + name;
    }
    return dir + "/" + name;
}

}  // namespace

void encodeLogEntry(const LogEntry& entry, net::Buffer& out) {
    WireWriter w(out);
    w.u64(entry.seq);
    w.u64(entry.term);
    if (const auto* n = std::get_if<NewOrder>(&entry.command)) {
        w.u8(0);
        w.u64(n->id);
        w.u32(n->instrument);
        w.u8(static_cast<std::uint8_t>(n->side));
        w.u8(static_cast<std::uint8_t>(n->type));
        w.u8(static_cast<std::uint8_t>(n->tif));
        w.i64(n->price);
        w.u64(n->quantity);
    } else {
        w.u8(1);
        w.u64(std::get<CancelOrder>(entry.command).id);
    }
}

std::optional<LogEntry> decodeLogEntry(const net::Byte* data, std::size_t size) {
    WireReader r(data, size);
    LogEntry entry;
    std::uint8_t tag = 0;
    if (!r.u64(entry.seq) || !r.u64(entry.term) || !r.u8(tag)) {
        return std::nullopt;
    }
    if (tag == 0) {
        NewOrder n;
        std::uint8_t side = 0;
        std::uint8_t type = 0;
        std::uint8_t tif = 0;
        if (!r.u64(n.id) || !r.u32(n.instrument) || !r.u8(side) || !r.u8(type) || !r.u8(tif) ||
            !r.i64(n.price) || !r.u64(n.quantity)) {
            return std::nullopt;
        }
        // Same validation as the wire decoder: bytes off a disk are no
        // more trustworthy than bytes off a socket.
        if (side > 1 || type > 1 || tif > 1) {
            return std::nullopt;
        }
        n.side = static_cast<Side>(side);
        n.type = static_cast<OrderType>(type);
        n.tif = static_cast<TimeInForce>(tif);
        entry.command = n;
    } else if (tag == 1) {
        CancelOrder c;
        if (!r.u64(c.id)) {
            return std::nullopt;
        }
        entry.command = c;
    } else {
        return std::nullopt;
    }
    if (!r.ok() || !r.exhausted()) {
        return std::nullopt;
    }
    return entry;
}

bool DurableState::open(const std::string& dir, SyncPolicy policy, std::string& error) {
    dir_ = dir;
    if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
        error = "cannot create " + dir;
        return false;
    }
    if (!stable_.open(join(dir, "state"), error)) {
        return false;
    }
    if (!wal_.open(join(dir, "log.wal"), policy, error)) {
        return false;
    }
    std::string snapError;
    snapshot_ = readSnapshotFile(join(dir, "snapshot"), snapError);
    if (!snapshot_.has_value() && !snapError.empty()) {
        // A corrupt snapshot is recoverable: fall back to replaying the
        // whole log. Slower, and correct, which is the right trade.
        snapshot_.reset();
    }
    return true;
}

bool DurableState::recover(MatchingEngine& engine, CommandLog& log, RecoveryReport& report,
                           std::string& error) {
    report = RecoveryReport{};
    report.currentTerm = stable_.state().currentTerm;
    report.votedFor = stable_.state().votedFor;
    report.tornTailDiscarded = wal_.stats().tailWasTorn;
    report.tornTailBytes = wal_.stats().truncatedTailBytes;

    Sequence startAfter = 0;
    if (snapshot_.has_value()) {
        if (!engine.restore(snapshot_->engine)) {
            error = "snapshot did not restore into this engine";
            return false;
        }
        startAfter = snapshot_->lastIncludedSeq;
        log.setSnapshotBoundary(snapshot_->lastIncludedSeq, snapshot_->lastIncludedTerm);
        report.hadSnapshot = true;
        report.snapshotSeq = snapshot_->lastIncludedSeq;
    }

    // Replay the WAL. Entries at or before the snapshot boundary are
    // already reflected in the restored engine, so they are skipped
    // rather than applied twice. Double-applying is the classic
    // snapshot-plus-log bug and it produces a book that is silently
    // wrong rather than obviously broken.
    EventList sink;
    for (const WalRecord& record : wal_.records()) {
        auto entry = decodeLogEntry(record.payload.data(), record.payload.size());
        if (!entry.has_value()) {
            // Should not happen: the WAL already verified CRCs. Stop
            // here rather than guessing at the rest.
            break;
        }
        if (entry->seq <= startAfter) {
            continue;
        }
        if (!log.appendAt(entry->seq, entry->term, entry->command)) {
            break;
        }
        sink.clear();
        engine.apply(entry->command, sink);
        ++report.entriesReplayed;
    }
    report.lastLogSeq = log.lastSeq();
    return true;
}

bool DurableState::appendEntry(const LogEntry& entry, std::string& error) {
    net::Buffer payload;
    encodeLogEntry(entry, payload);
    return wal_.append(static_cast<std::uint32_t>(entry.seq), payload.data(), payload.size(),
                       error);
}

bool DurableState::saveVote(std::uint64_t term, NodeId votedFor, std::string& error) {
    StableState next;
    next.currentTerm = term;
    next.votedFor = votedFor;
    return stable_.save(next, error);
}

bool DurableState::takeSnapshot(const MatchingEngine& engine, std::uint64_t lastIncludedSeq,
                                std::uint64_t lastIncludedTerm, std::string& error) {
    SnapshotRecord snap;
    snap.lastIncludedSeq = lastIncludedSeq;
    snap.lastIncludedTerm = lastIncludedTerm;
    snap.engine = engine.snapshot();
    snap.stateChecksum = engine.stateChecksum();

    // Snapshot first, THEN truncate the log. The other order loses data
    // if the process dies between the two: the log would be gone and the
    // snapshot would not exist yet.
    if (!writeSnapshotFile(join(dir_, "snapshot"), snap, error)) {
        return false;
    }
    snapshot_ = snap;
    // Keep anything the snapshot does not cover. Resetting the whole log
    // would discard entries after the snapshot point.
    return wal_.truncateThrough(static_cast<std::uint32_t>(lastIncludedSeq), error);
}

bool DurableState::installSnapshot(const SnapshotRecord& snap, MatchingEngine& engine,
                                   CommandLog& log, std::string& error) {
    if (!engine.restore(snap.engine)) {
        error = "received snapshot did not restore";
        return false;
    }
    if (engine.stateChecksum() != snap.stateChecksum) {
        error = "received snapshot restored to a different state than it claims";
        return false;
    }
    if (!writeSnapshotFile(join(dir_, "snapshot"), snap, error)) {
        return false;
    }
    snapshot_ = snap;
    log.clear();
    log.setSnapshotBoundary(snap.lastIncludedSeq, snap.lastIncludedTerm);
    return wal_.reset(error);
}

}  // namespace sententia::storage
