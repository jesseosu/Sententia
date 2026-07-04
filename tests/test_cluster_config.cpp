// Cluster config parsing, including the malformed cases that should
// stop a node rather than silently producing a smaller cluster.
#include "sententia/net/cluster_config.hpp"

#include "harness.hpp"

using namespace sententia::net;

namespace {

void run() {
    {
        ConfigError err;
        auto cfg = parseClusterConfig(
            "# comment\n"
            "node 1 127.0.0.1 7101\n"
            "\n"
            "node 2 127.0.0.1 7102   # trailing comment\n"
            "node 3 10.0.0.5 9000\n",
            err);
        CHECK(cfg.has_value());
        if (cfg.has_value()) {
            CHECK_EQ(cfg->peers.size(), std::size_t{3});
            CHECK_EQ(cfg->peers[0].id, NodeId{1});
            CHECK_STR_EQ(cfg->peers[2].host, "10.0.0.5");
            CHECK_EQ(cfg->peers[2].port, std::uint16_t{9000});

            CHECK(cfg->find(2) != nullptr);
            CHECK(cfg->find(99) == nullptr);

            const auto others = cfg->others(2);
            CHECK_EQ(others.size(), std::size_t{2});
            CHECK_EQ(others[0].id, NodeId{1});
            CHECK_EQ(others[1].id, NodeId{3});

            // A node not in the config gets everyone back.
            CHECK_EQ(cfg->others(42).size(), std::size_t{3});
        }
    }

    // An empty config is an error: a cluster of nobody is never intended.
    {
        ConfigError err;
        CHECK(!parseClusterConfig("# nothing here\n", err).has_value());
        CHECK(!parseClusterConfig("", err).has_value());
    }

    // Malformed lines stop the parse and report the line number.
    {
        ConfigError err;
        CHECK(!parseClusterConfig("node 1 127.0.0.1\n", err).has_value());
        CHECK_EQ(err.line, std::size_t{1});

        CHECK(!parseClusterConfig("node 1 127.0.0.1 7101\nbogus 2 x 1\n", err).has_value());
        CHECK_EQ(err.line, std::size_t{2});
    }

    // Ports out of range are rejected, including 0, which would mean
    // "let the kernel choose" and makes no sense for a peer address.
    {
        ConfigError err;
        CHECK(!parseClusterConfig("node 1 127.0.0.1 0\n", err).has_value());
        CHECK(!parseClusterConfig("node 1 127.0.0.1 70000\n", err).has_value());
    }

    // Duplicate ids are rejected: routing would be ambiguous, and the
    // ambiguity would not surface until two nodes disagreed.
    {
        ConfigError err;
        CHECK(
            !parseClusterConfig("node 1 127.0.0.1 7101\nnode 1 127.0.0.1 7102\n", err).has_value());
        CHECK_EQ(err.line, std::size_t{2});
    }
}

}  // namespace

TEST_MAIN("test_cluster_config")
