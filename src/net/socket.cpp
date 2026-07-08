#include "sententia/net/socket.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

namespace sententia::net {
namespace {

std::string errnoText(const char* what) {
    return std::string(what) + ": " + std::strerror(errno);
}

}  // namespace

Socket::~Socket() {
    close();
}

Socket::Socket(Socket&& other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

void Socket::close() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

int Socket::release() noexcept {
    const int fd = fd_;
    fd_ = -1;
    return fd;
}

bool Socket::setNonBlocking(std::string& error) const {
    const int flags = ::fcntl(fd_, F_GETFL, 0);
    if (flags < 0) {
        error = errnoText("fcntl(F_GETFL)");
        return false;
    }
    if (::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
        error = errnoText("fcntl(F_SETFL)");
        return false;
    }
    return true;
}

bool Socket::setSendBufferSize(int bytes, std::string& error) const {
    if (::setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes)) < 0) {
        error = errnoText("setsockopt(SO_SNDBUF)");
        return false;
    }
    return true;
}

bool Socket::setRecvBufferSize(int bytes, std::string& error) const {
    if (::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes)) < 0) {
        error = errnoText("setsockopt(SO_RCVBUF)");
        return false;
    }
    return true;
}

bool Socket::setNoDelay(std::string& error) const {
    int one = 1;
    if (::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) < 0) {
        error = errnoText("setsockopt(TCP_NODELAY)");
        return false;
    }
    return true;
}

bool Socket::setReuseAddr(std::string& error) const {
    int one = 1;
    if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
        error = errnoText("setsockopt(SO_REUSEADDR)");
        return false;
    }
    return true;
}

Socket Socket::listen(std::uint16_t port, int backlog, std::string& error) {
    Socket s(::socket(AF_INET, SOCK_STREAM, 0));
    if (!s.valid()) {
        error = errnoText("socket");
        return Socket{};
    }
    // Without SO_REUSEADDR a restarted node cannot rebind its port while
    // the previous socket sits in TIME_WAIT, which matters a great deal
    // once Phase 5 starts killing and restarting nodes.
    if (!s.setReuseAddr(error) || !s.setNonBlocking(error)) {
        return Socket{};
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (::bind(s.fd(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        error = errnoText("bind");
        return Socket{};
    }
    if (::listen(s.fd(), backlog) < 0) {
        error = errnoText("listen");
        return Socket{};
    }
    return s;
}

Socket Socket::accept(IoStatus& status) const {
    const int fd = ::accept(fd_, nullptr, nullptr);
    if (fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            status = IoStatus::WouldBlock;
        } else if (errno == EINTR || errno == ECONNABORTED) {
            // A peer that vanished between the poll and the accept.
            // Ordinary, not an error worth propagating.
            status = IoStatus::WouldBlock;
        } else {
            status = IoStatus::Error;
        }
        return Socket{};
    }
    status = IoStatus::Ok;
    return Socket(fd);
}

Socket Socket::connect(const std::string& host, std::uint16_t port, std::string& error) {
    Socket s(::socket(AF_INET, SOCK_STREAM, 0));
    if (!s.valid()) {
        error = errnoText("socket");
        return Socket{};
    }
    if (!s.setNonBlocking(error)) {
        return Socket{};
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        error = "not a valid IPv4 address: " + host;
        return Socket{};
    }

    if (::connect(s.fd(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        // EINPROGRESS is the expected result on a non-blocking socket:
        // the handshake is under way and completion shows up as the
        // socket becoming writable.
        if (errno != EINPROGRESS) {
            error = errnoText("connect");
            return Socket{};
        }
    }
    return s;
}

int Socket::connectError() const noexcept {
    int err = 0;
    socklen_t len = sizeof(err);
    if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &len) < 0) {
        return errno;
    }
    return err;
}

std::uint16_t Socket::localPort() const noexcept {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
        return 0;
    }
    return ntohs(addr.sin_port);
}

IoResult Socket::read(void* buf, std::size_t len) const {
    const ssize_t n = ::read(fd_, buf, len);
    if (n > 0) {
        return IoResult{IoStatus::Ok, static_cast<std::size_t>(n)};
    }
    // Zero from read() on a stream socket means the peer closed its end
    // in an orderly way. Not an error, but the connection is finished.
    if (n == 0) {
        return IoResult{IoStatus::Closed, 0};
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return IoResult{IoStatus::WouldBlock, 0};
    }
    return IoResult{IoStatus::Error, 0};
}

IoResult Socket::write(const void* buf, std::size_t len) const {
    // MSG_NOSIGNAL keeps a write to a closed peer from raising SIGPIPE
    // and killing the process. A peer disappearing must be an error
    // code, never a signal: in later phases peers die on purpose.
    const ssize_t n = ::send(fd_, buf, len, MSG_NOSIGNAL);
    if (n >= 0) {
        // A short write is normal, not an error. The caller keeps the
        // remainder and tries again when the socket is writable.
        return IoResult{IoStatus::Ok, static_cast<std::size_t>(n)};
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return IoResult{IoStatus::WouldBlock, 0};
    }
    if (errno == EPIPE || errno == ECONNRESET) {
        return IoResult{IoStatus::Closed, 0};
    }
    return IoResult{IoStatus::Error, 0};
}

}  // namespace sententia::net
