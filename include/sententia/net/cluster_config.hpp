// Sententia - static cluster membership.
//
// Service discovery is out of scope. A node reads a file listing every
// node in the cluster and their addresses. Dynamic membership is a
// genuinely hard distributed-systems problem in its own right, and
// solving it here would obscure the phases that actually need solving.
//
// Format, one node per line, '#' starts a comment:
//
//     node <id> <host> <port>
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "sententia/net/message.hpp"

namespace sententia::net {

struct PeerConfig {
    NodeId id{};
    std::string host;
    std::uint16_t port{};

    friend bool operator==(const PeerConfig&, const PeerConfig&) = default;
};

struct ClusterConfig {
    std::vector<PeerConfig> peers;

    const PeerConfig* find(NodeId id) const noexcept;

    // Every peer except `self`. The set a node should connect to.
    std::vector<PeerConfig> others(NodeId self) const;
};

struct ConfigError {
    std::size_t line{};
    std::string message;
};

// Parses cluster config text. Returns nullopt and fills `error` on the
// first malformed line, rather than silently skipping it: a typo in a
// node address should stop startup, not produce a cluster that is
// quietly missing a member.
std::optional<ClusterConfig> parseClusterConfig(const std::string& text, ConfigError& error);

std::optional<ClusterConfig> loadClusterConfig(const std::string& path, ConfigError& error);

}  // namespace sententia::net
