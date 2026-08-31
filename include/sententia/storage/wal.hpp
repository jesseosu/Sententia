// Sententia - write-ahead log.
//
// Everything before this phase lived in memory. A node that died lost
// its log, and a whole-cluster restart lost the venue. This is where
// state starts surviving power loss.
//
// The rule that gives the pattern its name: a command is written to disk
// and flushed BEFORE it is treated as committed. If the process dies at
// any instant, anything a client was told was committed is on disk, and
// anything not on disk was never acknowledged. That ordering is the
// whole guarantee, and getting it backwards is how databases lose data.
//
// TORN WRITES ARE THE INTERESTING PART.
//
// A crash mid-write leaves a partial record. The file is not corrupt in
// an obvious way; it ends in a record that is half there. Reading it
// back naively gives garbage that looks like data.
//
// So every record carries its length and a CRC of its payload. On open,
// records are replayed until one fails to verify, and the file is
// truncated at that point. A half-written record at the tail is normal
// after a crash, not an error: it is a command that was never
// acknowledged, so discarding it is correct.
//
// Record layout, 16-byte header then payload:
//
//   offset  size  field
//   0       4     magic    0x4C41575F, "_WAL"
//   4       4     seq      sequence number of the entry
//   8       4     length   payload byte count
//   12      4     crc32    of the payload only
//   16      ...   payload
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "sententia/net/wire.hpp"

namespace sententia::storage {

using sententia::net::Buffer;
using sententia::net::Byte;

constexpr std::uint32_t kWalMagic = 0x4C41575F;
constexpr std::size_t kWalHeaderSize = 16;
constexpr std::uint32_t kMaxRecordSize = 8u << 20;

// How hard the log tries to be on disk before returning.
enum class SyncPolicy {
    // fsync after every append. A command acknowledged is on the
    // platter. Slowest, and the only setting that survives a power cut
    // with zero loss.
    EveryWrite,
    // fsync every N appends. A crash can lose up to N commands, in
    // exchange for far fewer syscalls. The usual production choice, with
    // N tuned to how much loss the business can tolerate.
    Batched,
    // Never fsync. Data reaches the OS page cache and no further, so a
    // process crash is survivable but a power cut is not. For tests and
    // benchmarks, never for a real venue.
    Never,
};

const char* toString(SyncPolicy p) noexcept;

struct WalRecord {
    std::uint32_t seq{};
    Buffer payload;
};

struct WalStats {
    std::uint64_t appends{0};
    std::uint64_t fsyncs{0};
    std::uint64_t bytesWritten{0};
    std::uint64_t recordsReplayed{0};
    // Set when a partial record was found at the tail and discarded.
    // Expected after a crash, and worth surfacing rather than hiding.
    std::uint64_t truncatedTailBytes{0};
    bool tailWasTorn{false};
};

class WriteAheadLog {
public:
    WriteAheadLog() = default;
    ~WriteAheadLog();

    WriteAheadLog(const WriteAheadLog&) = delete;
    WriteAheadLog& operator=(const WriteAheadLog&) = delete;

    // Opens or creates the log, replaying what is there. Any torn record
    // at the tail is discarded and the file truncated to the last good
    // record. Returns false only on a real I/O failure; a torn tail is a
    // normal outcome, reported through stats.
    bool open(const std::string& path, SyncPolicy policy, std::string& error);

    // Reads every valid record. Call after open, before appending.
    const std::vector<WalRecord>& records() const noexcept { return records_; }

    // Appends and, depending on policy, flushes. Returns false on I/O
    // failure, which the caller must treat as "not committed".
    bool append(std::uint32_t seq, const Byte* data, std::size_t size, std::string& error);

    // Forces everything buffered to durable storage. Called explicitly
    // by a Batched caller before acknowledging a client.
    bool sync(std::string& error);

    void close();
    bool isOpen() const noexcept { return fd_ >= 0; }

    // Replaces the log with a fresh one containing nothing.
    bool reset(std::string& error);

    // Rewrites the log keeping only records after `seq`. Used when a
    // snapshot makes a PREFIX redundant while the log continues past it.
    //
    // Dropping the whole log instead would be wrong whenever the
    // snapshot point is behind the log head, and it also hides the
    // overlap case entirely: with no records at or before the boundary,
    // nothing ever tests that recovery skips them.
    bool truncateThrough(std::uint32_t seq, std::string& error);

    std::uint64_t sizeBytes() const noexcept { return bytesOnDisk_; }
    const WalStats& stats() const noexcept { return stats_; }
    SyncPolicy policy() const noexcept { return policy_; }
    void setBatchSize(std::size_t n) noexcept { batchSize_ = n; }

    const std::string& path() const noexcept { return path_; }

private:
    bool replay(std::string& error);

    int fd_{-1};
    std::string path_;
    SyncPolicy policy_{SyncPolicy::EveryWrite};
    std::size_t batchSize_{64};
    std::size_t sinceSync_{0};
    std::uint64_t bytesOnDisk_{0};
    std::vector<WalRecord> records_;
    WalStats stats_;
};

// CRC32 (IEEE polynomial), used to detect torn and corrupted records.
// Not cryptographic, and not meant to be: it defends against a disk that
// died mid-write, not against an attacker.
std::uint32_t crc32(const Byte* data, std::size_t size) noexcept;

}  // namespace sententia::storage
