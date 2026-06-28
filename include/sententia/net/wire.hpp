// Sententia - byte-level wire encoding primitives.
//
// Every integer crosses the network as explicit little-endian bytes,
// assembled and read back one byte at a time. Not memcpy of a struct,
// not htonl, not a reinterpret_cast over a buffer.
//
// The reason is the same one that made prices integer ticks in Phase 1.
// A replica has to decode a message to the identical value the sender
// encoded, on a different machine, possibly a different compiler, and
// eventually a different architecture. Anything that leaks the host's
// byte order, struct padding, or alignment rules into the wire format
// is a divergence waiting to happen. Shifting bytes explicitly is a few
// instructions slower and completely unambiguous.
//
// WireReader is bounds-checked on every read and never throws. A
// truncated or malformed message must be a clean decode failure, since
// the bytes arrive from the network and Phase 5 assumes peers can die
// mid-write.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace sententia::net {

using Byte = std::uint8_t;
using Buffer = std::vector<Byte>;

class WireWriter {
public:
    explicit WireWriter(Buffer& out) noexcept : out_(out) {}

    void u8(std::uint8_t v) { out_.push_back(v); }

    void u16(std::uint16_t v) {
        out_.push_back(static_cast<Byte>(v & 0xFF));
        out_.push_back(static_cast<Byte>((v >> 8) & 0xFF));
    }

    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            out_.push_back(static_cast<Byte>((v >> (i * 8)) & 0xFF));
        }
    }

    void u64(std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            out_.push_back(static_cast<Byte>((v >> (i * 8)) & 0xFF));
        }
    }

    // Signed values go over the wire as two's complement in the
    // unsigned encoding. Casting through uint64_t is well defined in
    // both directions; reinterpreting the object bytes would not be.
    void i64(std::int64_t v) { u64(static_cast<std::uint64_t>(v)); }

    // Length-prefixed so a string needs no terminator and cannot run
    // past the end of the message.
    void str(const std::string& s) {
        u32(static_cast<std::uint32_t>(s.size()));
        out_.insert(out_.end(), s.begin(), s.end());
    }

    std::size_t size() const noexcept { return out_.size(); }

private:
    Buffer& out_;
};

class WireReader {
public:
    WireReader(const Byte* data, std::size_t size) noexcept : data_(data), size_(size) {}

    explicit WireReader(const Buffer& buf) noexcept : data_(buf.data()), size_(buf.size()) {}

    // Every read returns false rather than throwing or reading past the
    // end. Once a read fails the reader stays failed, so a caller can
    // decode a whole message and check ok() once at the end instead of
    // testing every field.
    bool u8(std::uint8_t& out) noexcept {
        if (!need(1)) {
            return false;
        }
        out = data_[pos_++];
        return true;
    }

    bool u16(std::uint16_t& out) noexcept {
        if (!need(2)) {
            return false;
        }
        out = static_cast<std::uint16_t>(static_cast<std::uint16_t>(data_[pos_]) |
                                         (static_cast<std::uint16_t>(data_[pos_ + 1]) << 8));
        pos_ += 2;
        return true;
    }

    bool u32(std::uint32_t& out) noexcept {
        if (!need(4)) {
            return false;
        }
        out = 0;
        for (int i = 0; i < 4; ++i) {
            out |= static_cast<std::uint32_t>(data_[pos_ + static_cast<std::size_t>(i)]) << (i * 8);
        }
        pos_ += 4;
        return true;
    }

    bool u64(std::uint64_t& out) noexcept {
        if (!need(8)) {
            return false;
        }
        out = 0;
        for (int i = 0; i < 8; ++i) {
            out |= static_cast<std::uint64_t>(data_[pos_ + static_cast<std::size_t>(i)]) << (i * 8);
        }
        pos_ += 8;
        return true;
    }

    bool i64(std::int64_t& out) noexcept {
        std::uint64_t raw = 0;
        if (!u64(raw)) {
            return false;
        }
        out = static_cast<std::int64_t>(raw);
        return true;
    }

    bool str(std::string& out, std::uint32_t maxLen) noexcept {
        std::uint32_t len = 0;
        if (!u32(len)) {
            return false;
        }
        // A length field arriving from the network is untrusted. Cap it
        // before it becomes an allocation size.
        if (len > maxLen || !need(len)) {
            failed_ = true;
            return false;
        }
        out.assign(reinterpret_cast<const char*>(data_ + pos_), len);
        pos_ += len;
        return true;
    }

    bool ok() const noexcept { return !failed_; }
    std::size_t remaining() const noexcept { return size_ - pos_; }
    bool exhausted() const noexcept { return pos_ == size_; }

private:
    bool need(std::size_t n) noexcept {
        if (failed_ || size_ - pos_ < n) {
            failed_ = true;
            return false;
        }
        return true;
    }

    const Byte* data_{nullptr};
    std::size_t size_{0};
    std::size_t pos_{0};
    bool failed_{false};
};

}  // namespace sententia::net
