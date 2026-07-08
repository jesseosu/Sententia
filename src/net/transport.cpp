#include "sententia/net/transport.hpp"

#include <poll.h>

#include <algorithm>
#include <array>
#include <sstream>

namespace sententia::net {
namespace {

constexpr int kListenBacklog = 16;
constexpr std::size_t kReadChunk = 16 * 1024;

}  // namespace

const char* toString(PeerState s) noexcept {
    switch (s) {
        case PeerState::Disconnected:
            return "DISCONNECTED";
        case PeerState::Connecting:
            return "CONNECTING";
        case PeerState::Handshaking:
            return "HANDSHAKING";
        case PeerState::Ready:
            return "READY";
    }
    return "UNKNOWN";
}

Transport::Transport(NodeId selfId, std::uint16_t listenPort)
    : selfId_(selfId), listenPort_(listenPort) {}

Transport::~Transport() = default;

void Transport::log(const std::string& msg) const {
    if (onLog_) {
        onLog_(msg);
    }
}

bool Transport::start(std::string& error) {
    listener_ = Socket::listen(listenPort_, kListenBacklog, error);
    if (!listener_.valid()) {
        return false;
    }
    listenPort_ = listener_.localPort();
    log("listening on port " + std::to_string(listenPort_));
    return true;
}

std::uint16_t Transport::listenPort() const noexcept {
    return listenPort_;
}

void Transport::addPeer(const PeerConfig& cfg) {
    auto peer = std::make_unique<Peer>();
    peer->id = cfg.id;
    peer->host = cfg.host;
    peer->port = cfg.port;
    peer->outbound = true;
    peer->state = PeerState::Disconnected;
    peers_.push_back(std::move(peer));
    nextConnectCycle_.push_back(0);
}

void Transport::addPeers(const std::vector<PeerConfig>& list) {
    for (const PeerConfig& p : list) {
        addPeer(p);
    }
}

Peer* Transport::findPeer(NodeId id) noexcept {
    for (auto& p : peers_) {
        if (p->id == id && p->state == PeerState::Ready) {
            return p.get();
        }
    }
    return nullptr;
}

const Peer* Transport::findPeer(NodeId id) const noexcept {
    for (const auto& p : peers_) {
        if (p->id == id && p->state == PeerState::Ready) {
            return p.get();
        }
    }
    return nullptr;
}

bool Transport::isReady(NodeId id) const noexcept {
    return findPeer(id) != nullptr;
}

std::size_t Transport::readyPeerCount() const noexcept {
    std::size_t n = 0;
    for (const auto& p : peers_) {
        if (p->state == PeerState::Ready) {
            ++n;
        }
    }
    return n;
}

void Transport::sendHello(Peer& peer) {
    Hello hello;
    hello.nodeId = selfId_;
    hello.protocolVersion = kVersion;
    hello.advertisedAddress = "0.0.0.0:" + std::to_string(listenPort_);
    peer.writer.enqueue(Message{hello});
}

void Transport::startConnect(Peer& peer) {
    std::string error;
    peer.sock = Socket::connect(peer.host, peer.port, error);
    if (!peer.sock.valid()) {
        log("connect to node " + std::to_string(peer.id) + " failed: " + error);
        peer.state = PeerState::Disconnected;
        return;
    }
    peer.state = PeerState::Connecting;
    peer.reader.reset();
    peer.writer.reset();
}

void Transport::dropPeer(Peer& peer, const std::string& why) {
    const bool wasUp = peer.state == PeerState::Ready;
    peer.sock.close();
    peer.state = PeerState::Disconnected;
    peer.reader.reset();
    peer.writer.reset();
    ++stats_.disconnects;

    log("peer " + std::to_string(peer.id) + " down: " + why);
    if (wasUp && onPeerDown_) {
        onPeerDown_(peer.id, why);
    }

    // An inbound peer has no address to dial, so it is retired rather
    // than retried. The other side owns reconnection.
    if (!peer.outbound) {
        peer.id = 0;
    }
}

void Transport::acceptPending() {
    for (;;) {
        IoStatus status = IoStatus::Ok;
        Socket incoming = listener_.accept(status);
        if (status != IoStatus::Ok) {
            return;
        }
        std::string error;
        if (!incoming.setNonBlocking(error) || !incoming.setNoDelay(error)) {
            log("rejecting inbound connection: " + error);
            continue;
        }

        auto peer = std::make_unique<Peer>();
        peer->id = 0;  // unknown until Hello arrives
        peer->outbound = false;
        peer->state = PeerState::Handshaking;
        peer->sock = std::move(incoming);
        sendHello(*peer);
        peers_.push_back(std::move(peer));
        nextConnectCycle_.push_back(0);
        ++stats_.connectionsAccepted;
        log("accepted inbound connection");
    }
}

void Transport::handleReadable(Peer& peer) {
    std::array<Byte, kReadChunk> chunk{};
    for (;;) {
        const IoResult r = peer.sock.read(chunk.data(), chunk.size());
        if (r.status == IoStatus::WouldBlock) {
            break;
        }
        if (r.status == IoStatus::Closed) {
            dropPeer(peer, "peer closed connection");
            return;
        }
        if (r.status == IoStatus::Error) {
            dropPeer(peer, "read error");
            return;
        }

        stats_.bytesReceived += r.bytes;
        peer.reader.append(chunk.data(), r.bytes);
        // A read that fills the buffer probably has more waiting, so
        // loop. A short read means the socket is drained.
        if (r.bytes < chunk.size()) {
            ++stats_.partialReads;
            break;
        }
    }
    drainFrames(peer);
}

void Transport::drainFrames(Peer& peer) {
    for (;;) {
        if (peer.reader.failed()) {
            ++stats_.framingErrors;
            dropPeer(peer, std::string("framing error: ") + toString(peer.reader.status()));
            return;
        }
        auto frame = peer.reader.next();
        if (!frame.has_value()) {
            // Either no complete frame yet, or the read above tripped a
            // framing error which the top of the loop will catch.
            if (peer.reader.failed()) {
                continue;
            }
            return;
        }

        auto decoded = decodeBody(frame->type, frame->payload.data(), frame->payload.size());
        if (!decoded.has_value()) {
            // A message we cannot decode is not a framing error: the
            // frame boundary was correct, so the stream is still in
            // sync and the connection survives. Later phases will send
            // types this build does not know.
            ++stats_.decodeErrors;
            log("undecodable message type " + std::string(toString(frame->type)) + " from node " +
                std::to_string(peer.id));
            continue;
        }

        ++stats_.messagesReceived;

        // Hello is handled here rather than by the owner: it is how the
        // transport learns who is on the other end of an inbound socket.
        if (const auto* hello = std::get_if<Hello>(&decoded.value())) {
            if (hello->protocolVersion != kVersion) {
                dropPeer(peer, "protocol version mismatch");
                return;
            }
            if (peer.outbound && peer.id != hello->nodeId) {
                dropPeer(peer, "peer identified as node " + std::to_string(hello->nodeId) +
                                   " but config says " + std::to_string(peer.id));
                return;
            }

            // Duplicate connection resolution.
            //
            // In a mesh every node dials every other node, so each pair
            // ends up with two TCP connections: one in each direction.
            // Left alone, every message is delivered twice, which in
            // Phase 3 means applying every replicated command twice.
            //
            // The tiebreak has to be decidable by both sides
            // independently, with no negotiation, and reach the same
            // answer. The rule: keep the connection dialed by the
            // lower-numbered node. Node ids are unique and both sides
            // know both ids, so both compute the same winner.
            if (Peer* existing = findPeer(hello->nodeId);
                existing != nullptr && existing != &peer) {
                const bool keepOutbound = selfId_ < hello->nodeId;
                if (peer.outbound == keepOutbound) {
                    dropPeer(*existing, "duplicate connection, kept the other direction");
                } else {
                    dropPeer(peer, "duplicate connection, kept the other direction");
                    return;
                }
            }

            peer.id = hello->nodeId;
            peer.state = PeerState::Ready;
            log("peer " + std::to_string(peer.id) + " ready");
            if (onPeerUp_) {
                onPeerUp_(peer.id, hello->advertisedAddress);
            }
            continue;
        }

        // Anything before Hello is out of order and the peer is not
        // trustworthy yet.
        if (peer.state != PeerState::Ready) {
            dropPeer(peer, "message received before hello");
            return;
        }
        if (onMessage_) {
            onMessage_(peer.id, decoded.value());
        }
    }
}

void Transport::handleWritable(Peer& peer) {
    while (!peer.writer.empty()) {
        const IoResult r = peer.sock.write(peer.writer.data(), peer.writer.size());
        if (r.status == IoStatus::WouldBlock) {
            // Kernel buffer is full. Keep the remainder and wait for
            // the next POLLOUT. This is the normal backpressure path,
            // not an error.
            ++stats_.shortWrites;
            return;
        }
        if (r.status != IoStatus::Ok) {
            dropPeer(peer, "write failed");
            return;
        }
        if (r.bytes < peer.writer.size()) {
            ++stats_.shortWrites;
        }
        peer.writer.consume(r.bytes);
        stats_.bytesSent += r.bytes;
    }
}

bool Transport::poll(int timeoutMs) {
    ++cycle_;

    // Dial any outbound peer that is down and past its retry cycle.
    for (std::size_t i = 0; i < peers_.size(); ++i) {
        Peer& peer = *peers_[i];
        // Skip dialing a peer we already have a live connection to,
        // even if that connection is one the peer dialed to us. Without
        // this, the losing side of the duplicate-connection tiebreak
        // below would redial forever.
        if (peer.outbound && peer.state == PeerState::Disconnected &&
            cycle_ >= nextConnectCycle_[i] && !isReady(peer.id)) {
            nextConnectCycle_[i] = cycle_ + reconnectDelay_;
            startConnect(peer);
        }
    }

    std::vector<pollfd> fds;
    std::vector<Peer*> owners;
    fds.reserve(peers_.size() + 1);
    owners.reserve(peers_.size() + 1);

    if (listener_.valid()) {
        fds.push_back(pollfd{listener_.fd(), POLLIN, 0});
        owners.push_back(nullptr);
    }
    for (auto& p : peers_) {
        if (!p->sock.valid()) {
            continue;
        }
        short events = 0;
        if (p->state == PeerState::Connecting) {
            // A pending non-blocking connect completes by becoming
            // writable, success or failure alike.
            events = POLLOUT;
        } else {
            events = POLLIN;
            if (!p->writer.empty()) {
                events = static_cast<short>(events | POLLOUT);
            }
        }
        fds.push_back(pollfd{p->sock.fd(), events, 0});
        owners.push_back(p.get());
    }

    if (fds.empty()) {
        return true;
    }

    const int n = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), timeoutMs);
    if (n < 0) {
        // EINTR is a signal, not a failure. Let the caller poll again.
        return errno == EINTR;
    }
    if (n == 0) {
        return true;
    }

    for (std::size_t i = 0; i < fds.size(); ++i) {
        const short revents = fds[i].revents;
        if (revents == 0) {
            continue;
        }
        Peer* peer = owners[i];
        if (peer == nullptr) {
            acceptPending();
            continue;
        }

        if (peer->state == PeerState::Connecting) {
            const int err = peer->sock.connectError();
            if (err != 0) {
                dropPeer(*peer, "connect failed: " + std::string(std::strerror(err)));
                continue;
            }
            std::string error;
            peer->sock.setNoDelay(error);
            peer->state = PeerState::Handshaking;
            ++stats_.connectionsEstablished;
            sendHello(*peer);
            log("connected to node " + std::to_string(peer->id) + ", sending hello");
            handleWritable(*peer);
            continue;
        }

        // Check for hangup before reading so a half-closed socket is
        // still drained of whatever arrived before the close.
        if ((revents & (POLLERR | POLLNVAL)) != 0) {
            dropPeer(*peer, "socket error");
            continue;
        }
        if ((revents & POLLOUT) != 0) {
            handleWritable(*peer);
        }
        if (!peer->sock.valid()) {
            continue;  // dropped by the write path
        }
        if ((revents & (POLLIN | POLLHUP)) != 0) {
            handleReadable(*peer);
        }
    }

    // Retire inbound connections that have dropped: unlike outbound
    // peers there is nothing to reconnect to.
    peers_.erase(std::remove_if(peers_.begin(), peers_.end(),
                                [](const std::unique_ptr<Peer>& p) {
                                    return !p->outbound && p->state == PeerState::Disconnected;
                                }),
                 peers_.end());
    nextConnectCycle_.resize(peers_.size(), 0);
    return true;
}

bool Transport::send(NodeId to, const Message& m) {
    Peer* peer = findPeer(to);
    if (peer == nullptr) {
        return false;
    }
    peer->writer.enqueue(m);
    ++stats_.messagesSent;
    // Opportunistic write. If the socket is not ready the bytes stay
    // queued and go out on the next poll cycle.
    handleWritable(*peer);
    return true;
}

std::size_t Transport::broadcast(const Message& m) {
    std::size_t sent = 0;
    for (auto& p : peers_) {
        if (p->state == PeerState::Ready) {
            p->writer.enqueue(m);
            ++stats_.messagesSent;
            handleWritable(*p);
            ++sent;
        }
    }
    return sent;
}

}  // namespace sententia::net
