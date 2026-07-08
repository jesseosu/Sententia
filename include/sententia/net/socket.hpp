// Sententia - a thin RAII wrapper over a POSIX socket.
//
// Deliberately thin. It owns a file descriptor, closes it exactly once,
// and turns syscall return codes into a small result type. It does not
// buffer, frame, retry, or interpret. Everything above the byte level
// lives in framing.hpp, which is why that layer needs no sockets to be
// tested.
//
// POSIX only. The engine core stays portable and CI still builds and
// tests it on Windows and macOS, but the networking layer targets the
// platform this project actually deploys to. A Winsock shim would be
// real work for no signal, and Phase 5 pins threads with SCHED_FIFO on
// Linux anyway.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace sententia::net {

// Distinguishes the three outcomes that matter to a caller. "Would
// block" is not an error: on a non-blocking socket it is the normal way
// the kernel says "nothing more right now", and treating it as failure
// is the classic way to write a broken event loop.
enum class IoStatus {
    Ok,
    WouldBlock,
    Closed,  // orderly peer shutdown (read returned 0)
    Error,
};

struct IoResult {
    IoStatus status{IoStatus::Ok};
    std::size_t bytes{0};

    bool ok() const noexcept { return status == IoStatus::Ok; }
};

class Socket {
public:
    Socket() = default;
    explicit Socket(int fd) noexcept : fd_(fd) {}

    ~Socket();

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    bool valid() const noexcept { return fd_ >= 0; }
    int fd() const noexcept { return fd_; }
    void close() noexcept;
    int release() noexcept;

    // Binds and listens on the given port on all interfaces. Port 0
    // asks the kernel to pick one, which is what the tests use so they
    // never collide with a port something else is holding.
    static Socket listen(std::uint16_t port, int backlog, std::string& error);

    // Accepts one pending connection. Returns an invalid socket with
    // status WouldBlock when there is nothing to accept.
    Socket accept(IoStatus& status) const;

    // Starts a non-blocking connect. Completion is observed by polling
    // for writability, so this returns before the handshake finishes.
    static Socket connect(const std::string& host, std::uint16_t port, std::string& error);

    // Reports the error left on a socket after a non-blocking connect
    // completes. Zero means the connection succeeded.
    int connectError() const noexcept;

    // The port actually bound, which matters when port 0 was requested.
    std::uint16_t localPort() const noexcept;

    IoResult read(void* buf, std::size_t len) const;
    IoResult write(const void* buf, std::size_t len) const;

    bool setNonBlocking(std::string& error) const;

    // Socket buffer sizing. Real tuning knobs in a latency-sensitive
    // system, and the lever the backpressure test uses to make a short
    // write happen on demand rather than hoping volume produces one.
    bool setSendBufferSize(int bytes, std::string& error) const;
    bool setRecvBufferSize(int bytes, std::string& error) const;
    bool setNoDelay(std::string& error) const;
    bool setReuseAddr(std::string& error) const;

private:
    int fd_{-1};
};

}  // namespace sententia::net
