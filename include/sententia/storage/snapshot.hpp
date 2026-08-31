// Sententia - engine snapshots on disk, and why they exist.
//
// Recovery by replaying the whole log works, and gets slower every day
// the venue runs. A log with fifty million commands in it means fifty
// million commands to replay before the node is useful again.
//
// A snapshot is the engine state as of some sequence number. Recovery
// then means: load the newest snapshot, replay only the commands after
// it. Recovery time stops being a function of total history and becomes
// a function of how long since the last snapshot, which is a number you
// control.
//
// The file carries the state checksum it was taken at. On load, the
// restored engine's checksum is recomputed and compared. A snapshot that
// does not reproduce its own checksum is refused rather than trusted,
// because silently restoring a subtly wrong book is far worse than
// failing to start.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "sententia/engine.hpp"
#include "sententia/net/wire.hpp"

namespace sententia::storage {

using sententia::EngineSnapshot;
using sententia::net::Buffer;
using sententia::net::Byte;

// A snapshot plus where it sits in the replicated log.
struct SnapshotRecord {
    // The last log entry included in this snapshot, and its term. A
    // recovering node replays from lastIncludedSeq + 1.
    std::uint64_t lastIncludedSeq{0};
    std::uint64_t lastIncludedTerm{0};
    // Recomputed on load and compared. A mismatch means the snapshot is
    // not what it claims to be.
    std::uint64_t stateChecksum{0};
    EngineSnapshot engine;
};

// Pure serialisation, no I/O. Same explicit little-endian discipline as
// the wire protocol: a snapshot written on one machine must load on
// another.
void encodeSnapshot(const SnapshotRecord& snap, Buffer& out);
Buffer encodeSnapshot(const SnapshotRecord& snap);
std::optional<SnapshotRecord> decodeSnapshot(const Byte* data, std::size_t size);

// Writes atomically: temp file, fsync, rename, fsync the directory. A
// crash mid-snapshot must never leave a half-written snapshot where a
// good one used to be.
bool writeSnapshotFile(const std::string& path, const SnapshotRecord& snap, std::string& error);

// Loads and verifies. Returns nullopt if absent, corrupt, or if the
// restored state does not reproduce the recorded checksum.
std::optional<SnapshotRecord> readSnapshotFile(const std::string& path, std::string& error);

}  // namespace sententia::storage
