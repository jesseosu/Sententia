#include "sententia/net/framing.hpp"

#include <algorithm>

namespace sententia::net {
namespace {

// Once consumed bytes exceed this, slide the remainder to the front.
// Without compaction a long-lived connection's buffer grows without
// bound even though the live portion stays small.
constexpr std::size_t kCompactThreshold = 64 * 1024;

}  // namespace

void FrameReader::append(const Byte* data, std::size_t size) {
    if (failed() || size == 0) {
        return;
    }
    buf_.insert(buf_.end(), data, data + size);
}

void FrameReader::compact() {
    if (readPos_ == 0) {
        return;
    }
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(readPos_));
    readPos_ = 0;
}

std::optional<Frame> FrameReader::next() {
    if (failed()) {
        return std::nullopt;
    }

    // Not even a full header yet. Perfectly normal.
    if (buffered() < kHeaderSize) {
        return std::nullopt;
    }

    WireReader r(buf_.data() + readPos_, buffered());
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint16_t rawType = 0;
    std::uint32_t length = 0;
    // buffered() >= kHeaderSize was checked above, so these cannot fail.
    r.u32(magic);
    r.u16(version);
    r.u16(rawType);
    r.u32(length);

    if (magic != kMagic) {
        status_ = Status::BadMagic;
        return std::nullopt;
    }
    if (version != kVersion) {
        status_ = Status::BadVersion;
        return std::nullopt;
    }
    // Checked before it is used as an allocation size.
    if (length > kMaxPayload) {
        status_ = Status::PayloadTooLarge;
        return std::nullopt;
    }

    // Header is complete but the body has not fully arrived. Wait.
    if (buffered() < kHeaderSize + length) {
        return std::nullopt;
    }

    Frame frame;
    frame.type = static_cast<MessageType>(rawType);
    const Byte* body = buf_.data() + readPos_ + kHeaderSize;
    frame.payload.assign(body, body + length);

    readPos_ += kHeaderSize + length;
    if (readPos_ >= kCompactThreshold) {
        compact();
    }
    return frame;
}

void FrameReader::reset() noexcept {
    buf_.clear();
    readPos_ = 0;
    status_ = Status::Ok;
}

const char* toString(FrameReader::Status s) noexcept {
    switch (s) {
        case FrameReader::Status::Ok:
            return "OK";
        case FrameReader::Status::BadMagic:
            return "BAD_MAGIC";
        case FrameReader::Status::BadVersion:
            return "BAD_VERSION";
        case FrameReader::Status::PayloadTooLarge:
            return "PAYLOAD_TOO_LARGE";
    }
    return "UNKNOWN";
}

void FrameWriter::enqueue(const Message& m) {
    encode(m, buf_);
}

void FrameWriter::enqueueRaw(const Byte* data, std::size_t size) {
    buf_.insert(buf_.end(), data, data + size);
}

void FrameWriter::consume(std::size_t n) {
    writePos_ += std::min(n, size());
    if (writePos_ == buf_.size()) {
        // Fully drained: the common case, and the cheapest reset.
        buf_.clear();
        writePos_ = 0;
    } else if (writePos_ >= kCompactThreshold) {
        compact();
    }
}

void FrameWriter::compact() {
    if (writePos_ == 0) {
        return;
    }
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(writePos_));
    writePos_ = 0;
}

void FrameWriter::reset() noexcept {
    buf_.clear();
    writePos_ = 0;
}

}  // namespace sententia::net
