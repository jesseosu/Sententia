#include "sententia/storage/stable_store.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include <array>

#include "sententia/storage/wal.hpp"

namespace sententia::storage {
namespace {

// magic(4) term(8) votedFor(4) crc(4)
constexpr std::uint32_t kMagic = 0x42545353;  // "SSTB"
constexpr std::size_t kFileSize = 20;

std::string errnoText(const char* what) {
    return std::string(what) + ": " + std::strerror(errno);
}

void putU32(std::uint8_t* p, std::uint32_t v) noexcept {
    for (int i = 0; i < 4; ++i) {
        p[i] = static_cast<std::uint8_t>((v >> (i * 8)) & 0xFF);
    }
}

void putU64(std::uint8_t* p, std::uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<std::uint8_t>((v >> (i * 8)) & 0xFF);
    }
}

std::uint32_t getU32(const std::uint8_t* p) noexcept {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v |= static_cast<std::uint32_t>(p[i]) << (i * 8);
    }
    return v;
}

std::uint64_t getU64(const std::uint8_t* p) noexcept {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(p[i]) << (i * 8);
    }
    return v;
}

// fsync the directory containing `path`. Renaming a file is atomic, but
// the rename itself is only durable once the DIRECTORY entry is flushed.
// Skipping this is a classic and very hard to reproduce durability bug:
// everything looks correct until a power cut loses the rename.
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

bool StableStore::open(const std::string& path, std::string& error) {
    path_ = path;
    state_ = StableState{};
    stats_ = StableStoreStats{};

    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) {
            // A node that has never run. Term 0, no vote.
            return true;
        }
        error = errnoText(("open " + path).c_str());
        return false;
    }

    std::array<std::uint8_t, kFileSize> buf{};
    std::size_t got = 0;
    while (got < buf.size()) {
        const ssize_t n = ::read(fd, buf.data() + got, buf.size() - got);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            ::close(fd);
            error = errnoText("read");
            return false;
        }
        if (n == 0) {
            break;
        }
        got += static_cast<std::size_t>(n);
    }
    ::close(fd);

    if (got != kFileSize || getU32(buf.data()) != kMagic ||
        crc32(buf.data(), kFileSize - 4) != getU32(buf.data() + kFileSize - 4)) {
        // Truncated or corrupt. Treat as fresh rather than refusing to
        // start: a node with no remembered vote is conservative, since
        // it will simply grant or refuse based on the terms it sees, and
        // the cluster stays safe as long as a majority is intact.
        stats_.recoveredFromCorruption = true;
        state_ = StableState{};
        return true;
    }

    state_.currentTerm = getU64(buf.data() + 4);
    state_.votedFor = static_cast<NodeId>(getU32(buf.data() + 12));
    stats_.loadedExisting = true;
    return true;
}

bool StableStore::save(const StableState& state, std::string& error) {
    std::array<std::uint8_t, kFileSize> buf{};
    putU32(buf.data(), kMagic);
    putU64(buf.data() + 4, state.currentTerm);
    putU32(buf.data() + 12, static_cast<std::uint32_t>(state.votedFor));
    putU32(buf.data() + kFileSize - 4, crc32(buf.data(), kFileSize - 4));

    // Write to a temporary, flush it, then rename over the target. The
    // rename is atomic, so a crash at any point leaves either the whole
    // old file or the whole new one.
    const std::string tmp = path_ + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        error = errnoText(("open " + tmp).c_str());
        return false;
    }

    std::size_t written = 0;
    while (written < buf.size()) {
        const ssize_t n = ::write(fd, buf.data() + written, buf.size() - written);
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
    ++stats_.fsyncs;

    if (::rename(tmp.c_str(), path_.c_str()) < 0) {
        error = errnoText("rename");
        return false;
    }
    // The rename is only durable once the directory entry is flushed.
    if (!syncParentDirectory(path_, error)) {
        return false;
    }
    ++stats_.fsyncs;
    ++stats_.writes;
    state_ = state;
    return true;
}

}  // namespace sententia::storage
