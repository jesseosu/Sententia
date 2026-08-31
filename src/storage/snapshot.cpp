#include "sententia/storage/snapshot.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

#include <array>

#include "sententia/storage/wal.hpp"

namespace sententia::storage {
namespace {

constexpr std::uint32_t kMagic = 0x50414E53;  // "SNAP"
constexpr std::uint16_t kVersion = 1;
constexpr std::uint32_t kMaxOrders = 50u << 20;

using sententia::net::WireReader;
using sententia::net::WireWriter;

std::string errnoText(const char* what) {
    return std::string(what) + ": " + std::strerror(errno);
}

bool syncParentDirectory(const std::string& path, std::string& error) {
    const auto slash = path.find_last_of('/');
    const std::string dir = slash == std::string::npos ? std::string(".") : path.substr(0, slash);
    const int dfd = ::open(dir.c_str(), O_RDONLY);
    if (dfd < 0) {
        error = errnoText(("open directory " + dir).c_str());
        return false;
    }
    const bool ok = ::fsync(dfd) == 0;
    if (!ok) {
        error = errnoText("fsync directory");
    }
    ::close(dfd);
    return ok;
}

}  // namespace

void encodeSnapshot(const SnapshotRecord& snap, Buffer& out) {
    WireWriter w(out);
    w.u32(kMagic);
    w.u16(kVersion);
    w.u64(snap.lastIncludedSeq);
    w.u64(snap.lastIncludedTerm);
    w.u64(snap.stateChecksum);

    w.u32(snap.engine.instrument);
    w.u64(snap.engine.commandSeq);
    w.u64(snap.engine.eventSeq);
    w.u64(snap.engine.arrivalCounter);

    w.u8(snap.engine.lastTop.hasBid ? 1 : 0);
    w.i64(snap.engine.lastTop.bidPrice);
    w.u64(snap.engine.lastTop.bidQuantity);
    w.u8(snap.engine.lastTop.hasAsk ? 1 : 0);
    w.i64(snap.engine.lastTop.askPrice);
    w.u64(snap.engine.lastTop.askQuantity);

    w.u32(static_cast<std::uint32_t>(snap.engine.orders.size()));
    for (const sententia::RestingOrder& o : snap.engine.orders) {
        w.u64(o.id);
        w.u8(static_cast<std::uint8_t>(o.side));
        w.i64(o.price);
        w.u64(o.quantity);
        w.u64(o.arrival);
    }
}

Buffer encodeSnapshot(const SnapshotRecord& snap) {
    Buffer out;
    encodeSnapshot(snap, out);
    return out;
}

std::optional<SnapshotRecord> decodeSnapshot(const Byte* data, std::size_t size) {
    WireReader r(data, size);
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    SnapshotRecord snap;
    if (!r.u32(magic) || !r.u16(version) || magic != kMagic || version != kVersion) {
        return std::nullopt;
    }
    if (!r.u64(snap.lastIncludedSeq) || !r.u64(snap.lastIncludedTerm) ||
        !r.u64(snap.stateChecksum)) {
        return std::nullopt;
    }
    if (!r.u32(snap.engine.instrument) || !r.u64(snap.engine.commandSeq) ||
        !r.u64(snap.engine.eventSeq) || !r.u64(snap.engine.arrivalCounter)) {
        return std::nullopt;
    }

    std::uint8_t hasBid = 0;
    std::uint8_t hasAsk = 0;
    if (!r.u8(hasBid) || !r.i64(snap.engine.lastTop.bidPrice) ||
        !r.u64(snap.engine.lastTop.bidQuantity) || !r.u8(hasAsk) ||
        !r.i64(snap.engine.lastTop.askPrice) || !r.u64(snap.engine.lastTop.askQuantity)) {
        return std::nullopt;
    }
    if (hasBid > 1 || hasAsk > 1) {
        return std::nullopt;
    }
    snap.engine.lastTop.hasBid = hasBid == 1;
    snap.engine.lastTop.hasAsk = hasAsk == 1;

    std::uint32_t count = 0;
    if (!r.u32(count) || count > kMaxOrders) {
        return std::nullopt;
    }
    snap.engine.orders.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        sententia::RestingOrder o;
        std::uint8_t side = 0;
        if (!r.u64(o.id) || !r.u8(side) || !r.i64(o.price) || !r.u64(o.quantity) ||
            !r.u64(o.arrival)) {
            return std::nullopt;
        }
        if (side > 1) {
            return std::nullopt;
        }
        o.side = static_cast<sententia::Side>(side);
        snap.engine.orders.push_back(o);
    }
    if (!r.ok() || !r.exhausted()) {
        return std::nullopt;
    }
    return snap;
}

bool writeSnapshotFile(const std::string& path, const SnapshotRecord& snap, std::string& error) {
    Buffer body = encodeSnapshot(snap);
    // A trailing CRC, so a torn snapshot is detected rather than loaded.
    Buffer file = body;
    const std::uint32_t sum = crc32(body.data(), body.size());
    for (int i = 0; i < 4; ++i) {
        file.push_back(static_cast<Byte>((sum >> (i * 8)) & 0xFF));
    }

    const std::string tmp = path + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        error = errnoText(("open " + tmp).c_str());
        return false;
    }
    std::size_t written = 0;
    while (written < file.size()) {
        const ssize_t n = ::write(fd, file.data() + written, file.size() - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            error = errnoText("write");
            ::close(fd);
            return false;
        }
        written += static_cast<std::size_t>(n);
    }
    if (::fsync(fd) < 0) {
        error = errnoText("fsync");
        ::close(fd);
        return false;
    }
    ::close(fd);
    if (::rename(tmp.c_str(), path.c_str()) < 0) {
        error = errnoText("rename");
        return false;
    }
    return syncParentDirectory(path, error);
}

std::optional<SnapshotRecord> readSnapshotFile(const std::string& path, std::string& error) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        if (errno != ENOENT) {
            error = errnoText(("open " + path).c_str());
        }
        return std::nullopt;
    }
    Buffer file;
    std::array<Byte, 64 * 1024> chunk{};
    for (;;) {
        const ssize_t n = ::read(fd, chunk.data(), chunk.size());
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            error = errnoText("read");
            ::close(fd);
            return std::nullopt;
        }
        if (n == 0) {
            break;
        }
        file.insert(file.end(), chunk.begin(), chunk.begin() + n);
    }
    ::close(fd);

    if (file.size() < 4) {
        error = "snapshot too small";
        return std::nullopt;
    }
    const std::size_t bodySize = file.size() - 4;
    std::uint32_t stored = 0;
    for (int i = 0; i < 4; ++i) {
        stored |= static_cast<std::uint32_t>(file[bodySize + static_cast<std::size_t>(i)])
                  << (i * 8);
    }
    if (crc32(file.data(), bodySize) != stored) {
        error = "snapshot checksum mismatch (torn or corrupt file)";
        return std::nullopt;
    }

    auto snap = decodeSnapshot(file.data(), bodySize);
    if (!snap.has_value()) {
        error = "snapshot failed to decode";
        return std::nullopt;
    }

    // Verify the snapshot actually reproduces the state it claims. A
    // snapshot that restores to a different book than it recorded is
    // worse than no snapshot, because it looks like it worked.
    sententia::MatchingEngine probe(snap->engine.instrument);
    if (!probe.restore(snap->engine)) {
        error = "snapshot did not restore cleanly";
        return std::nullopt;
    }
    if (probe.stateChecksum() != snap->stateChecksum) {
        error = "snapshot restored to a different state than recorded";
        return std::nullopt;
    }
    return snap;
}

}  // namespace sententia::storage
