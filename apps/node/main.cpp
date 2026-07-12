// Sententia - cluster node.
//
// The two-node heartbeat demo, and the skeleton every later phase grows
// into. It starts a transport, connects to its configured peers, and
// exchanges heartbeats, logging every message.
//
// The matching engine is deliberately absent. Wiring it in is Phase 3's
// job, and doing it now would mean guessing at a replication design
// before the transport it runs on has been proven.
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "sententia/net/cluster_config.hpp"
#include "sententia/net/transport.hpp"

using namespace sententia::net;

namespace {

volatile std::sig_atomic_t g_stop = 0;

void onSignal(int) {
    g_stop = 1;
}

void usage(const char* argv0) {
    std::cerr << "usage: " << argv0 << " --id <node-id> --config <cluster.conf>\n"
              << "                    [--heartbeat-cycles N] [--max-cycles N] [--quiet]\n";
}

}  // namespace

int main(int argc, char** argv) {
    NodeId selfId = 0;
    std::string configPath;
    int heartbeatCycles = 10;
    long maxCycles = -1;
    bool quiet = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--id" && i + 1 < argc) {
            selfId = static_cast<NodeId>(std::strtoul(argv[++i], nullptr, 10));
        } else if (arg == "--config" && i + 1 < argc) {
            configPath = argv[++i];
        } else if (arg == "--heartbeat-cycles" && i + 1 < argc) {
            heartbeatCycles = static_cast<int>(std::strtol(argv[++i], nullptr, 10));
        } else if (arg == "--max-cycles" && i + 1 < argc) {
            maxCycles = std::strtol(argv[++i], nullptr, 10);
        } else if (arg == "--quiet") {
            quiet = true;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (selfId == 0 || configPath.empty()) {
        usage(argv[0]);
        return 2;
    }

    ConfigError configError;
    auto config = loadClusterConfig(configPath, configError);
    if (!config.has_value()) {
        std::cerr << configPath << ":" << configError.line << ": " << configError.message << "\n";
        return 1;
    }

    const PeerConfig* self = config->find(selfId);
    if (self == nullptr) {
        std::cerr << "node id " << selfId << " is not in " << configPath << "\n";
        return 1;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    Transport transport(selfId, self->port);
    if (!quiet) {
        transport.onLog([selfId](const std::string& msg) {
            std::cout << "[node " << selfId << "] " << msg << std::endl;
        });
    }
    transport.onPeerUp([selfId](NodeId id, const std::string& addr) {
        std::cout << "[node " << selfId << "] PEER UP " << id << " at " << addr << std::endl;
    });
    transport.onPeerDown([selfId](NodeId id, const std::string& why) {
        std::cout << "[node " << selfId << "] PEER DOWN " << id << ": " << why << std::endl;
    });
    transport.onMessage([selfId](NodeId from, const Message& m) {
        std::cout << "[node " << selfId << "] RECV from " << from << ": " << describe(m)
                  << std::endl;
    });

    std::string error;
    if (!transport.start(error)) {
        std::cerr << "failed to start: " << error << "\n";
        return 1;
    }
    transport.addPeers(config->others(selfId));

    std::uint64_t heartbeatCounter = 0;
    long cycles = 0;

    while (g_stop == 0 && (maxCycles < 0 || cycles < maxCycles)) {
        if (!transport.poll(100)) {
            std::cerr << "poll failed\n";
            return 1;
        }
        ++cycles;
        if (heartbeatCycles > 0 && cycles % heartbeatCycles == 0) {
            Heartbeat hb;
            hb.nodeId = selfId;
            hb.counter = ++heartbeatCounter;
            transport.broadcast(Message{hb});
        }
    }

    const TransportStats& s = transport.stats();
    std::cout << "[node " << selfId << "] shutting down\n"
              << "  messages_sent=" << s.messagesSent << "\n"
              << "  messages_received=" << s.messagesReceived << "\n"
              << "  bytes_sent=" << s.bytesSent << "\n"
              << "  bytes_received=" << s.bytesReceived << "\n"
              << "  short_writes=" << s.shortWrites << "\n"
              << "  connections_accepted=" << s.connectionsAccepted << "\n"
              << "  connections_established=" << s.connectionsEstablished << "\n"
              << "  disconnects=" << s.disconnects << "\n"
              << "  framing_errors=" << s.framingErrors << std::endl;
    return 0;
}
