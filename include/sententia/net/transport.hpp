// Sententia - poll()-based TCP transport for a small cluster.
//
// Owns a listening socket and a set of peer connections, multiplexes
// them with poll(), and turns the byte stream on each into framed
// messages via FrameReader / FrameWriter.
//
// poll() rather than epoll(): a cluster is three to five nodes, so the
// fd set is tiny. poll()'s cost is linear in the number of descriptors,
// which is irrelevant at this size, and it is portable across every
// POSIX system including macOS. epoll() wins at hundreds or thousands of
// connections by not rescanning the whole set each call. That is a real
// difference at scale and no difference at all here. Revisit if a node
// ever fans out to many clients.
//
// The transport does no reconnection backoff timing of its own beyond a
// retry interval, and holds no threads: poll() blocks with a timeout and
// the owner drives it. Keeping the concurrency model boring is
// deliberate, because the interesting concurrency arrives in Phase 4.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "sententia/net/cluster_config.hpp"
#include "sententia/net/framing.hpp"
#include "sententia/net/message.hpp"
#include "sententia/net/socket.hpp"

namespace sententia::net {

// Why a send did or did not go through. Phase 2 returned a bare bool,
// which conflated "no such peer" with "peer is saturated" and left the
// caller no way to apply backpressure. Replication needs to tell them
// apart: the first is a routing problem, the second is a flow-control
// signal that the caller must act on.
enum class SendResult {
    Ok,
    NotConnected,
    WouldOverflow,
};

const char* toString(SendResult r) noexcept;

enum class PeerState {
    Disconnected,
    Connecting,   // non-blocking connect in flight
    Handshaking,  // TCP up, Hello not yet exchanged
    Ready,
};

const char* toString(PeerState s) noexcept;

struct TransportStats {
    std::uint64_t messagesSent{0};
    std::uint64_t messagesReceived{0};
    std::uint64_t bytesSent{0};
    std::uint64_t bytesReceived{0};
    std::uint64_t shortWrites{0};
    std::uint64_t partialReads{0};
    std::uint64_t connectionsAccepted{0};
    std::uint64_t connectionsEstablished{0};
    std::uint64_t disconnects{0};
    std::uint64_t framingErrors{0};
    std::uint64_t decodeErrors{0};
    std::uint64_t sendsRefusedOverflow{0};
};

// One connection. Either dialled by us or accepted from a peer.
struct Peer {
    NodeId id{};  // 0 until Hello identifies an inbound peer
    std::string host;
    std::uint16_t port{};
    bool outbound{false};  // we dialled it, so we redial it if it drops
    PeerState state{PeerState::Disconnected};
    Socket sock;
    FrameReader reader;
    FrameWriter writer;
};

class Transport {
public:
    using MessageHandler = std::function<void(NodeId from, const Message&)>;
    using PeerEventHandler = std::function<void(NodeId id, const std::string& detail)>;
    using LogHandler = std::function<void(const std::string&)>;

    Transport(NodeId selfId, std::uint16_t listenPort);
    ~Transport();

    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;

    // Binds the listening socket. Must succeed before poll() is useful.
    bool start(std::string& error);

    // Registers a peer to dial. Connection is attempted lazily inside
    // poll() and retried after a drop.
    void addPeer(const PeerConfig& peer);
    void addPeers(const std::vector<PeerConfig>& peers);

    // Runs one poll cycle: accepts, completes connects, reads, writes,
    // and dispatches. `timeoutMs` of -1 blocks indefinitely, 0 returns
    // immediately. Returns false only on an unrecoverable error.
    bool poll(int timeoutMs);

    // Queues a message. Never blocks; the bytes leave on a later poll().
    // Refuses rather than queueing without bound once the peer's unsent
    // backlog reaches its high-water mark.
    SendResult send(NodeId to, const Message& m);

    // Queues to every ready peer. Returns how many peers it reached.
    std::size_t broadcast(const Message& m);

    // Unsent bytes queued for a peer, and whether it is saturated. The
    // replicator polls these to decide whether it may issue more work.
    std::size_t pendingBytes(NodeId id) const noexcept;
    bool isSaturated(NodeId id) const noexcept;

    // Applies a high-water mark to every current and future peer.
    void setHighWaterMark(std::size_t bytes) noexcept;

    // Forcibly drops a peer connection. An outbound peer is redialled
    // on a later cycle, exactly as if the link had failed. Operationally
    // this is "force a reconnect"; in tests it is how a network fault is
    // injected without needing a real one.
    bool disconnectPeer(NodeId id, const std::string& reason);

    void onMessage(MessageHandler h) { onMessage_ = std::move(h); }
    void onPeerUp(PeerEventHandler h) { onPeerUp_ = std::move(h); }
    void onPeerDown(PeerEventHandler h) { onPeerDown_ = std::move(h); }
    void onLog(LogHandler h) { onLog_ = std::move(h); }

    NodeId selfId() const noexcept { return selfId_; }
    // The port actually bound. Differs from the requested port when 0
    // was passed, which is how the tests avoid fixed-port collisions.
    std::uint16_t listenPort() const noexcept;
    std::size_t readyPeerCount() const noexcept;
    bool isReady(NodeId id) const noexcept;
    const TransportStats& stats() const noexcept { return stats_; }

    // Retry interval for dropped outbound connections, in poll cycles.
    void setReconnectDelayCycles(int cycles) noexcept { reconnectDelay_ = cycles; }

private:
    void log(const std::string& msg) const;
    void acceptPending();
    void startConnect(Peer& peer);
    void handleReadable(Peer& peer);
    void handleWritable(Peer& peer);
    void drainFrames(Peer& peer);
    void dropPeer(Peer& peer, const std::string& why);
    void sendHello(Peer& peer);
    Peer* findPeer(NodeId id) noexcept;
    const Peer* findPeer(NodeId id) const noexcept;

    NodeId selfId_{};
    std::uint16_t listenPort_{};
    Socket listener_;
    std::vector<std::unique_ptr<Peer>> peers_;
    std::size_t highWaterMark_{kDefaultHighWaterMark};
    int reconnectDelay_{10};
    int cycle_{0};
    std::vector<int> nextConnectCycle_;

    MessageHandler onMessage_;
    PeerEventHandler onPeerUp_;
    PeerEventHandler onPeerDown_;
    LogHandler onLog_;
    TransportStats stats_;
};

}  // namespace sententia::net
