// Sententia - cluster node.
//
// A node owns a matching engine, a command log, a transport, and a
// replicator. A primary accepts commands from an order file and
// replicates them; a backup applies whatever the primary sends.
//
// The demo worth running: start a backup, then a primary pointed at the
// same order file the single-process replay driver uses, and watch both
// print the same state checksum at the end. Neither ever sent the other
// a book.
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <chrono>

#include "sententia/consensus/election.hpp"
#include "sententia/net/cluster_config.hpp"
#include "sententia/net/transport.hpp"
#include "sententia/replication/replicator.hpp"

using namespace sententia;
using namespace sententia::consensus;
using namespace sententia::net;
using namespace sententia::replication;

namespace {

volatile std::sig_atomic_t g_stop = 0;

void onSignal(int) {
    g_stop = 1;
}

void usage(const char* argv0) {
    std::cerr << "usage: " << argv0 << " --id <node-id> --config <cluster.conf>\n"
              << "  [--elect]                 elect a leader instead of designating one\n"
              << "  [--role primary|backup]   ignored under --elect; default backup\n"
              << "  [--mode sync|async]       default sync\n"
              << "  [--orders <file>]         primary: replicate this order file\n"
              << "  [--sync-window N]         default 8\n"
              << "  [--heartbeat-cycles N]    default 20, 0 disables\n"
              << "  [--max-cycles N]          safety net: exit after N poll cycles\n"
              << "  [--exit-on-complete]      exit when the work is actually done\n"
              << "  [--quiet]\n";
}

bool parseSide(const std::string& s, Side& out) {
    if (s == "BUY" || s == "B") {
        out = Side::Buy;
        return true;
    }
    if (s == "SELL" || s == "S") {
        out = Side::Sell;
        return true;
    }
    return false;
}
bool parseType(const std::string& s, OrderType& out) {
    if (s == "LIMIT" || s == "L") {
        out = OrderType::Limit;
        return true;
    }
    if (s == "MARKET" || s == "M") {
        out = OrderType::Market;
        return true;
    }
    return false;
}
bool parseTif(const std::string& s, TimeInForce& out) {
    if (s == "GTC") {
        out = TimeInForce::GoodTillCancel;
        return true;
    }
    if (s == "IOC") {
        out = TimeInForce::ImmediateOrCancel;
        return true;
    }
    return false;
}

// Same format as the replay driver, so the same file drives both.
bool loadOrders(const std::string& path, InstrumentId instrument, std::vector<Command>& out,
                std::string& error) {
    std::ifstream file(path);
    if (!file) {
        error = "cannot open " + path;
        return false;
    }
    std::string raw;
    std::size_t lineNo = 0;
    while (std::getline(file, raw)) {
        ++lineNo;
        const auto hash = raw.find('#');
        if (hash != std::string::npos) {
            raw.erase(hash);
        }
        std::istringstream line(raw);
        std::string kind;
        if (!(line >> kind)) {
            continue;
        }
        if (kind == "N" || kind == "NEW") {
            NewOrder o;
            std::string sideText, typeText, tifText;
            if (!(line >> o.id >> sideText >> typeText >> tifText >> o.price >> o.quantity) ||
                !parseSide(sideText, o.side) || !parseType(typeText, o.type) ||
                !parseTif(tifText, o.tif)) {
                error = path + ":" + std::to_string(lineNo) + ": malformed order";
                return false;
            }
            o.instrument = instrument;
            out.push_back(o);
        } else if (kind == "C" || kind == "CANCEL") {
            CancelOrder c;
            if (!(line >> c.id)) {
                error = path + ":" + std::to_string(lineNo) + ": malformed cancel";
                return false;
            }
            out.push_back(c);
        } else {
            error = path + ":" + std::to_string(lineNo) + ": unknown command " + kind;
            return false;
        }
    }
    return true;
}

constexpr InstrumentId kInstrument = 1;

}  // namespace

int main(int argc, char** argv) {
    NodeId selfId = 0;
    std::string configPath;
    std::string ordersPath;
    Role role = Role::Backup;
    ReplicationMode mode = ReplicationMode::Synchronous;
    std::size_t syncWindow = 8;
    int heartbeatCycles = 20;
    long maxCycles = -1;
    bool quiet = false;
    bool exitOnComplete = false;
    bool elect = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--id" && i + 1 < argc) {
            selfId = static_cast<NodeId>(std::strtoul(argv[++i], nullptr, 10));
        } else if (arg == "--config" && i + 1 < argc) {
            configPath = argv[++i];
        } else if (arg == "--orders" && i + 1 < argc) {
            ordersPath = argv[++i];
        } else if (arg == "--role" && i + 1 < argc) {
            const std::string v = argv[++i];
            if (v == "primary") {
                role = Role::Primary;
            } else if (v == "backup") {
                role = Role::Backup;
            } else {
                usage(argv[0]);
                return 2;
            }
        } else if (arg == "--mode" && i + 1 < argc) {
            const std::string v = argv[++i];
            if (v == "sync") {
                mode = ReplicationMode::Synchronous;
            } else if (v == "async") {
                mode = ReplicationMode::Asynchronous;
            } else {
                usage(argv[0]);
                return 2;
            }
        } else if (arg == "--sync-window" && i + 1 < argc) {
            syncWindow = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--heartbeat-cycles" && i + 1 < argc) {
            heartbeatCycles = static_cast<int>(std::strtol(argv[++i], nullptr, 10));
        } else if (arg == "--max-cycles" && i + 1 < argc) {
            maxCycles = std::strtol(argv[++i], nullptr, 10);
        } else if (arg == "--elect") {
            elect = true;
        } else if (arg == "--exit-on-complete") {
            exitOnComplete = true;
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

    std::vector<Command> orders;
    if (!ordersPath.empty()) {
        std::string error;
        if (!loadOrders(ordersPath, kInstrument, orders, error)) {
            std::cerr << error << "\n";
            return 1;
        }
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    MatchingEngine engine(kInstrument);
    CommandLog log;
    Transport transport(selfId, self->port);
    // Under --elect nobody starts as primary. The cluster decides.
    Replicator replicator(selfId, elect ? Role::Backup : role, mode, engine, log, transport);
    replicator.setSyncWindow(syncWindow);

    std::vector<NodeId> peerIds;
    for (const PeerConfig& p : config->others(selfId)) {
        peerIds.push_back(p.id);
    }
    ElectionConfig electionConfig;
    // Seeded from the node id so two nodes never share a timeout
    // schedule, and so a run is reproducible.
    electionConfig.randomSeed = 0x9E3779B9ULL * (selfId + 1);
    Election election(selfId, peerIds, electionConfig);

    // The clock lives here, at the edge, and is passed into the election
    // as a parameter. The election itself never reads it, which is what
    // lets the whole cluster be simulated on a virtual clock in tests.
    const auto startedAt = std::chrono::steady_clock::now();
    const auto nowMillis = [&startedAt]() -> Millis {
        return static_cast<Millis>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - startedAt)
                                       .count());
    };

    const auto tag = [selfId, &election, &replicator, elect] {
        const std::string what = elect ? std::string(toString(election.role()))
                                       : std::string(toString(replicator.role()));
        return "[" + what + " " + std::to_string(selfId) + "] ";
    };

    if (!quiet) {
        transport.onLog([&](const std::string& m) { std::cout << tag() << m << std::endl; });
        replicator.onLog([&](const std::string& m) { std::cout << tag() << m << std::endl; });
    }
    transport.onPeerUp([&](NodeId id, const std::string& addr) {
        std::cout << tag() << "PEER UP " << id << " at " << addr << std::endl;
        replicator.onPeerUp(id);
    });
    transport.onPeerDown([&](NodeId id, const std::string& why) {
        std::cout << tag() << "PEER DOWN " << id << ": " << why << std::endl;
        replicator.onPeerDown(id);
    });
    // Performs whatever the election asked for. The election never
    // sends anything itself.
    const auto runActions = [&](const ActionList& actions) {
        for (const Action& a : actions) {
            switch (a.kind) {
                case Action::Kind::SendRequestVote: {
                    RequestVote rv;
                    rv.term = a.term;
                    rv.candidateId = selfId;
                    rv.lastLogSeq = a.lastLogSeq;
                    transport.send(a.to, Message{rv});
                    break;
                }
                case Action::Kind::SendVoteResponse: {
                    VoteResponse vr;
                    vr.term = a.term;
                    vr.voterId = selfId;
                    vr.granted = a.voteGranted;
                    transport.send(a.to, Message{vr});
                    break;
                }
                case Action::Kind::SendHeartbeat: {
                    // An empty AppendEntries is the heartbeat, exactly as
                    // in Raft. Unifying them means there is no separate
                    // liveness path that could disagree with the
                    // replication path about who leads.
                    AppendEntries ae;
                    ae.term = a.term;
                    ae.leaderId = selfId;
                    ae.prevSeq = log.lastSeq();
                    ae.commitSeq = replicator.commitSeq();
                    transport.send(a.to, Message{ae});
                    break;
                }
                case Action::Kind::BecameLeader:
                    std::cout << tag() << "ELECTED LEADER for term " << a.term << std::endl;
                    replicator.setTerm(a.term);
                    replicator.setRole(Role::Primary);
                    for (const NodeId p : peerIds) {
                        if (transport.isReady(p)) {
                            replicator.onPeerUp(p);
                        }
                    }
                    break;
                case Action::Kind::SteppedDown:
                    std::cout << tag() << "STEPPED DOWN at term " << a.term << std::endl;
                    replicator.setTerm(a.term);
                    replicator.setRole(Role::Backup);
                    break;
            }
        }
    };

    transport.onMessage([&](NodeId from, const Message& m) {
        // Replication traffic goes to the replicator; anything else is
        // logged. Heartbeats fall in the second group, which is what
        // keeps the Phase 2 two-node demo meaningful now that this
        // binary also does replication.
        const MessageType type = typeOf(m);
        ActionList actions;
        if (elect) {
            // Election first: it owns the term, and the replicator must
            // see the updated term before acting on the same message.
            if (const auto* rv = std::get_if<RequestVote>(&m)) {
                election.setLastLogSeq(log.lastSeq());
                election.onRequestVote(*rv, nowMillis(), actions);
                runActions(actions);
                return;
            }
            if (const auto* vr = std::get_if<VoteResponse>(&m)) {
                election.onVoteResponse(*vr, nowMillis(), actions);
                runActions(actions);
                return;
            }
            if (const auto* ae = std::get_if<AppendEntries>(&m)) {
                election.onAppendEntries(ae->term, ae->leaderId, nowMillis(), actions);
                runActions(actions);
                replicator.setTerm(election.currentTerm());
            } else if (const auto* ar = std::get_if<AppendResponse>(&m)) {
                election.onAppendResponse(ar->term, nowMillis(), actions);
                runActions(actions);
                replicator.setTerm(election.currentTerm());
            }
        }
        if (type == MessageType::AppendEntries || type == MessageType::AppendResponse) {
            replicator.onMessage(from, m);
            return;
        }
        if (!quiet) {
            std::cout << tag() << "RECV from " << from << ": " << describe(m) << std::endl;
        }
    });

    std::string error;
    if (!transport.start(error)) {
        std::cerr << "failed to start: " << error << "\n";
        return 1;
    }
    transport.addPeers(config->others(selfId));

    std::cout << tag() << "mode=" << toString(mode) << " sync_window=" << syncWindow
              << " orders=" << orders.size() << std::endl;

    std::size_t nextOrder = 0;
    std::uint64_t heartbeatCounter = 0;
    long cycles = 0;
    bool everConnected = false;
    bool complete = false;

    while (g_stop == 0 && !complete && (maxCycles < 0 || cycles < maxCycles)) {
        transport.poll(10);
        if (elect) {
            ActionList actions;
            election.setLastLogSeq(log.lastSeq());
            election.tick(nowMillis(), actions);
            runActions(actions);
            replicator.setTerm(election.currentTerm());
        }
        replicator.tick();
        ++cycles;

        // Feed the order file through as the primary. Busy is normal
        // backpressure, so the order is simply retried next cycle.
        if (replicator.role() == Role::Primary && nextOrder < orders.size()) {
            for (int burst = 0; burst < 256 && nextOrder < orders.size(); ++burst) {
                const SubmitResult r = replicator.submit(orders[nextOrder]);
                if (!r.ok()) {
                    break;
                }
                ++nextOrder;
            }
            if (nextOrder == orders.size()) {
                std::cout << tag() << "all " << orders.size() << " orders submitted" << std::endl;
            }
        }

        if (heartbeatCycles > 0 && cycles % heartbeatCycles == 0) {
            Heartbeat hb;
            hb.nodeId = selfId;
            hb.counter = ++heartbeatCounter;
            transport.broadcast(Message{hb});
        }

        // Exit on the work being finished rather than on a cycle budget.
        //
        // A fixed --max-cycles budget can run out before convergence on
        // a loaded machine, which makes a demo that is really a test
        // fail for reasons unrelated to the code. That is the same
        // mistake as asserting a short write on loopback: bounding a
        // test by something the environment controls. The budget stays
        // as a safety net so nothing can hang, but it is no longer what
        // normally ends the run.
        if (transport.readyPeerCount() > 0) {
            everConnected = true;
        }
        if (exitOnComplete) {
            if (replicator.role() == Role::Primary) {
                // Everything submitted, applied locally, and confirmed
                // held by every backup.
                complete = everConnected && nextOrder == orders.size() &&
                           replicator.lastApplied() == log.lastSeq() &&
                           replicator.commitSeq() == log.lastSeq() && log.lastSeq() > 0;
            } else {
                // The primary finished and went away.
                complete = everConnected && transport.readyPeerCount() == 0;
            }
        }
    }

    const ReplicationStats& rs = replicator.stats();
    const TransportStats& ts = transport.stats();
    std::cout << tag() << "shutting down\n"
              << "  role=" << toString(replicator.role()) << " mode=" << toString(mode) << "\n"
              << "  election_role=" << toString(election.role()) << "\n"
              << "  term=" << election.currentTerm() << "\n"
              << "  elections_started=" << election.stats().electionsStarted << "\n"
              << "  elections_won=" << election.stats().electionsWon << "\n"
              << "  votes_granted=" << election.stats().votesGranted << "\n"
              << "  step_downs=" << election.stats().stepDowns << "\n"
              << "  last_applied=" << replicator.lastApplied() << "\n"
              << "  commit_seq=" << replicator.commitSeq() << "\n"
              << "  log_last_seq=" << log.lastSeq() << "\n"
              << "  submitted=" << rs.submitted << "\n"
              << "  applied=" << rs.applied << "\n"
              << "  entries_sent=" << rs.entriesSent << "\n"
              << "  entries_received=" << rs.entriesReceived << "\n"
              << "  gaps_detected=" << rs.gapsDetected << "\n"
              << "  checksum_mismatches=" << rs.checksumMismatches << "\n"
              << "  resting_orders=" << engine.book().orderCount() << "\n"
              << "  messages_sent=" << ts.messagesSent << "\n"
              << "  messages_received=" << ts.messagesReceived << "\n"
              << "  bytes_sent=" << ts.bytesSent << "\n"
              << "  sends_refused_overflow=" << ts.sendsRefusedOverflow << "\n"
              << "  disconnects=" << ts.disconnects << "\n"
              << "  framing_errors=" << ts.framingErrors << "\n"
              << "  state_checksum=" << engine.stateChecksum() << std::endl;
    return 0;
}
