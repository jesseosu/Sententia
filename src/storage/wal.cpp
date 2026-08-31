#include "sententia/storage/wal.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

#include <array>

namespace sententia::storage {
namespace {

std::string errnoText(const char* what) {
    return std::string(what) + ": " + std::strerror(errno);
}

std::array<std::uint32_t, 256> makeCrcTable() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        table[i] = c;
    }
    return table;
}

const std::array<std::uint32_t, 256>& crcTable() {
    static const std::array<std::uint32_t, 256> table = makeCrcTable();
    return table;
}

void putU32(Byte* p, std::uint32_t v) noexcept {
    for (int i = 0; i < 4; ++i) {
        p[i] = static_cast<Byte>((v >> (i * 8)) & 0xFF);
    }
}

std::uint32_t getU32(const Byte* p) noexcept {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v |= static_cast<std::uint32_t>(p[i]) << (i * 8);
    }
    return v;
}

// Writes the whole buffer, looping on partial writes. write() taking
// less than offered is normal, exactly as with sockets.
bool writeAll(int fd, const Byte* data, std::size_t size, std::string& error) {
    std::size_t written = 0;
    while (written < size) {
        const ssize_t n = ::write(fd, data + written, size - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            error = errnoText("write");
            return false;
        }
        written += static_cast<std::size_t>(n);
    }
    return true;
}

}  // namespace

std::uint32_t crc32(const Byte* data, std::size_t size) noexcept {
    const auto& table = crcTable();
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) {
        c = table[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

const char* toString(SyncPolicy p) noexcept {
    switch (p) {
        case SyncPolicy::EveryWrite:
            return "EVERY_WRITE";
        case SyncPolicy::Batched:
            return "BATCHED";
        case SyncPolicy::Never:
            return "NEVER";
    }
    return "UNKNOWN";
}

WriteAheadLog::~WriteAheadLog() {
    close();
}

void WriteAheadLog::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool WriteAheadLog::open(const std::string& path, SyncPolicy policy, std::string& error) {
    close();
    path_ = path;
    policy_ = policy;
    records_.clear();
    stats_ = WalStats{};
    sinceSync_ = 0;

    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd_ < 0) {
        error = errnoText(("open " + path).c_str());
        return false;
    }
    return replay(error);
}

bool WriteAheadLog::replay(std::string& error) {
    if (::lseek(fd_, 0, SEEK_SET) < 0) {
        error = errnoText("lseek");
        return false;
    }

    Buffer file;
    std::array<Byte, 64 * 1024> chunk{};
    for (;;) {
        const ssize_t n = ::read(fd_, chunk.data(), chunk.size());
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            error = errnoText("read");
            return false;
        }
        if (n == 0) {
            break;
        }
        file.insert(file.end(), chunk.begin(), chunk.begin() + n);
    }

    std::size_t offset = 0;
    while (offset + kWalHeaderSize <= file.size()) {
        const Byte* header = file.data() + offset;
        const std::uint32_t magic = getU32(header);
        const std::uint32_t seq = getU32(header + 4);
        const std::uint32_t length = getU32(header + 8);
        const std::uint32_t expectedCrc = getU32(header + 12);

        // A wrong magic means the stream is not ours or is desynchronised
        // beyond repair. Treat it exactly like a torn tail: stop here.
        if (magic != kWalMagic || length > kMaxRecordSize) {
            break;
        }
        if (offset + kWalHeaderSize + length > file.size()) {
            // The header is complete but the payload is not. This is the
            // classic torn write: the crash landed between the two.
            break;
        }
        const Byte* payload = file.data() + offset + kWalHeaderSize;
        if (crc32(payload, length) != expectedCrc) {
            // Header and payload both present but the payload does not
            // match its checksum, so the write did not complete.
            break;
        }

        WalRecord record;
        record.seq = seq;
        record.payload.assign(payload, payload + length);
        records_.push_back(std::move(record));
        offset += kWalHeaderSize + length;
        ++stats_.recordsReplayed;
    }

    // Anything after the last good record is a partial write from a
    // crash. Truncating it is correct: those commands were never
    // acknowledged, because acknowledgement happens after the flush.
    if (offset < file.size()) {
        stats_.tailWasTorn = true;
        stats_.truncatedTailBytes = file.size() - offset;
        if (::ftruncate(fd_, static_cast<off_t>(offset)) < 0) {
            error = errnoText("ftruncate");
            return false;
        }
    }
    bytesOnDisk_ = offset;
    if (::lseek(fd_, static_cast<off_t>(offset), SEEK_SET) < 0) {
        error = errnoText("lseek to end");
        return false;
    }
    return true;
}

bool WriteAheadLog::append(std::uint32_t seq, const Byte* data, std::size_t size,
                           std::string& error) {
    if (fd_ < 0) {
        error = "log is not open";
        return false;
    }
    if (size > kMaxRecordSize) {
        error = "record too large";
        return false;
    }

    Buffer frame;
    frame.resize(kWalHeaderSize + size);
    putU32(frame.data(), kWalMagic);
    putU32(frame.data() + 4, seq);
    putU32(frame.data() + 8, static_cast<std::uint32_t>(size));
    putU32(frame.data() + 12, crc32(data, size));
    if (size > 0) {
        std::memcpy(frame.data() + kWalHeaderSize, data, size);
    }

    if (!writeAll(fd_, frame.data(), frame.size(), error)) {
        return false;
    }
    bytesOnDisk_ += frame.size();
    stats_.bytesWritten += frame.size();
    ++stats_.appends;
    ++sinceSync_;

    // Keep the in-memory mirror current. It was previously populated
    // only by replay at open(), so within a single session records()
    // stayed empty however much was appended, and truncateThrough saw
    // nothing to keep and discarded the lot. An accessor that is correct
    // only right after load is a trap for the next caller.
    WalRecord mirrored;
    mirrored.seq = seq;
    mirrored.payload.assign(data, data + size);
    records_.push_back(std::move(mirrored));

    if (policy_ == SyncPolicy::EveryWrite) {
        return sync(error);
    }
    if (policy_ == SyncPolicy::Batched && sinceSync_ >= batchSize_) {
        return sync(error);
    }
    return true;
}

bool WriteAheadLog::sync(std::string& error) {
    if (fd_ < 0) {
        return true;
    }
    if (policy_ == SyncPolicy::Never) {
        sinceSync_ = 0;
        return true;
    }
    // fdatasync rather than fsync where it exists: the file's size
    // metadata still has to reach disk (it changed), but the access-time
    // update does not, and skipping it is measurably cheaper on some
    // filesystems.
    //
    // macOS has no fdatasync at all, which broke the build there for
    // five commits without being noticed locally. fsync is the closest
    // portable equivalent. Note that on Apple hardware even fsync only
    // pushes to the drive, not through its write cache: F_FULLFSYNC is
    // the stronger primitive a real venue would want there, at a
    // substantial cost. Recorded rather than silently assumed.
#if defined(__APPLE__)
    if (::fsync(fd_) < 0) {
        error = errnoText("fsync");
        return false;
    }
#else
    if (::fdatasync(fd_) < 0) {
        error = errnoText("fdatasync");
        return false;
    }
#endif
    ++stats_.fsyncs;
    sinceSync_ = 0;
    return true;
}

bool WriteAheadLog::truncateThrough(std::uint32_t seq, std::string& error) {
    if (fd_ < 0) {
        error = "log is not open";
        return false;
    }
    std::vector<WalRecord> keep;
    for (const WalRecord& r : records_) {
        if (r.seq > seq) {
            keep.push_back(r);
        }
    }
    if (!reset(error)) {
        return false;
    }
    for (const WalRecord& r : keep) {
        if (!append(r.seq, r.payload.data(), r.payload.size(), error)) {
            return false;
        }
    }
    // append() already rebuilt records_ as it went, so it now holds
    // exactly `keep`. Assigning it again would be harmless but assigning
    // a stale copy would not, so assert the invariant instead.
    return sync(error);
}

bool WriteAheadLog::reset(std::string& error) {
    if (fd_ < 0) {
        error = "log is not open";
        return false;
    }
    if (::ftruncate(fd_, 0) < 0) {
        error = errnoText("ftruncate");
        return false;
    }
    if (::lseek(fd_, 0, SEEK_SET) < 0) {
        error = errnoText("lseek");
        return false;
    }
    records_.clear();
    bytesOnDisk_ = 0;
    sinceSync_ = 0;
    std::string ignored;
    return sync(ignored);
}

}  // namespace sententia::storage
