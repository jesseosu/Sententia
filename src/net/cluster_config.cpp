#include "sententia/net/cluster_config.hpp"

#include <fstream>
#include <set>
#include <sstream>

namespace sententia::net {

const PeerConfig* ClusterConfig::find(NodeId id) const noexcept {
    for (const PeerConfig& p : peers) {
        if (p.id == id) {
            return &p;
        }
    }
    return nullptr;
}

std::vector<PeerConfig> ClusterConfig::others(NodeId self) const {
    std::vector<PeerConfig> out;
    for (const PeerConfig& p : peers) {
        if (p.id != self) {
            out.push_back(p);
        }
    }
    return out;
}

std::optional<ClusterConfig> parseClusterConfig(const std::string& text, ConfigError& error) {
    ClusterConfig config;
    std::istringstream in(text);
    std::string raw;
    std::size_t lineNo = 0;
    std::set<NodeId> seenIds;

    while (std::getline(in, raw)) {
        ++lineNo;
        const auto hash = raw.find('#');
        if (hash != std::string::npos) {
            raw.erase(hash);
        }
        std::istringstream line(raw);
        std::string keyword;
        if (!(line >> keyword)) {
            continue;  // blank or comment-only
        }
        if (keyword != "node") {
            error = {lineNo, "unknown directive: " + keyword};
            return std::nullopt;
        }

        PeerConfig peer;
        unsigned long id = 0;
        unsigned long port = 0;
        if (!(line >> id >> peer.host >> port)) {
            error = {lineNo, "expected: node <id> <host> <port>"};
            return std::nullopt;
        }
        if (port == 0 || port > 65535) {
            error = {lineNo, "port out of range: " + std::to_string(port)};
            return std::nullopt;
        }
        peer.id = static_cast<NodeId>(id);
        peer.port = static_cast<std::uint16_t>(port);

        // Duplicate ids would make routing ambiguous, and the ambiguity
        // would not surface until two nodes disagreed about who a
        // message was for.
        if (!seenIds.insert(peer.id).second) {
            error = {lineNo, "duplicate node id: " + std::to_string(peer.id)};
            return std::nullopt;
        }
        config.peers.push_back(peer);
    }

    if (config.peers.empty()) {
        error = {0, "cluster config lists no nodes"};
        return std::nullopt;
    }
    return config;
}

std::optional<ClusterConfig> loadClusterConfig(const std::string& path, ConfigError& error) {
    std::ifstream file(path);
    if (!file) {
        error = {0, "cannot open " + path};
        return std::nullopt;
    }
    std::ostringstream buf;
    buf << file.rdbuf();
    return parseClusterConfig(buf.str(), error);
}

}  // namespace sententia::net
