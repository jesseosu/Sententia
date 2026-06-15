// Sententia replay driver.
//
// Reads a command file, applies it to a fresh engine, and prints the
// resulting event stream plus the final state checksum. This is the I/O
// edge: file parsing, stream writing, and argument handling all live
// here so that none of it can leak into the engine core.
//
// The output is byte-stable for a given input file, so
//
//     replay orders.txt > a.txt && replay orders.txt > b.txt && diff a.txt b.txt
//
// is a one-line demonstration of the determinism property, and diffing
// two nodes' output is how divergence gets caught in later phases.
//
// Command file format, one per line, '#' starts a comment:
//
//     N <id> <BUY|SELL> <LIMIT|MARKET> <GTC|IOC> <price> <qty>
//     C <id>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "sententia/engine.hpp"

using namespace sententia;

namespace {

constexpr InstrumentId kDefaultInstrument = 1;

struct ParseError {
    std::size_t line;
    std::string message;
};

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

bool parse(std::istream& in, InstrumentId instrument, std::vector<Command>& out,
           ParseError& error) {
    std::string raw;
    std::size_t lineNo = 0;

    while (std::getline(in, raw)) {
        ++lineNo;
        const auto hash = raw.find('#');
        if (hash != std::string::npos) {
            raw.erase(hash);
        }
        std::istringstream line(raw);
        std::string kind;
        if (!(line >> kind)) {
            continue;  // blank or comment-only
        }

        if (kind == "N" || kind == "NEW") {
            NewOrder order;
            std::string sideText;
            std::string typeText;
            std::string tifText;
            if (!(line >> order.id >> sideText >> typeText >> tifText >> order.price >>
                  order.quantity)) {
                error = {lineNo, "expected: N <id> <side> <type> <tif> <price> <qty>"};
                return false;
            }
            if (!parseSide(sideText, order.side)) {
                error = {lineNo, "bad side: " + sideText};
                return false;
            }
            if (!parseType(typeText, order.type)) {
                error = {lineNo, "bad type: " + typeText};
                return false;
            }
            if (!parseTif(tifText, order.tif)) {
                error = {lineNo, "bad time in force: " + tifText};
                return false;
            }
            order.instrument = instrument;
            out.push_back(order);
        } else if (kind == "C" || kind == "CANCEL") {
            CancelOrder cancel;
            if (!(line >> cancel.id)) {
                error = {lineNo, "expected: C <id>"};
                return false;
            }
            out.push_back(cancel);
        } else {
            error = {lineNo, "unknown command: " + kind};
            return false;
        }
    }
    return true;
}

void printBook(const OrderBook& book, std::ostream& os) {
    os << "book bids:\n";
    for (const auto& [price, queue] : book.bids()) {
        Quantity total = 0;
        for (const RestingOrder& o : queue) {
            total += o.quantity;
        }
        os << "  " << price << " x" << total << " (" << queue.size() << " orders)\n";
    }
    os << "book asks:\n";
    for (const auto& [price, queue] : book.asks()) {
        Quantity total = 0;
        for (const RestingOrder& o : queue) {
            total += o.quantity;
        }
        os << "  " << price << " x" << total << " (" << queue.size() << " orders)\n";
    }
}

void usage(const char* argv0) {
    std::cerr << "usage: " << argv0 << " [--instrument N] [--quiet] <command-file>\n"
              << "       " << argv0 << " [--instrument N] [--quiet] -\n"
              << "reads '-' as stdin\n";
}

}  // namespace

int main(int argc, char** argv) {
    InstrumentId instrument = kDefaultInstrument;
    bool quiet = false;
    std::string path;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--instrument" && i + 1 < argc) {
            instrument = static_cast<InstrumentId>(std::strtoul(argv[++i], nullptr, 10));
        } else if (arg == "--quiet") {
            quiet = true;
        } else if (arg == "-h" || arg == "--help") {
            usage(argv[0]);
            return 0;
        } else if (path.empty()) {
            path = arg;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (path.empty()) {
        usage(argv[0]);
        return 2;
    }

    std::vector<Command> commands;
    ParseError error{};
    bool ok = false;
    if (path == "-") {
        ok = parse(std::cin, instrument, commands, error);
    } else {
        std::ifstream file(path);
        if (!file) {
            std::cerr << "cannot open " << path << "\n";
            return 1;
        }
        ok = parse(file, instrument, commands, error);
    }

    if (!ok) {
        std::cerr << path << ":" << error.line << ": " << error.message << "\n";
        return 1;
    }

    MatchingEngine engine(instrument);
    EventList events;
    events.reserve(commands.size() * 3);
    for (const Command& c : commands) {
        engine.apply(c, events);
    }

    if (!quiet) {
        for (const Event& e : events) {
            std::cout << toString(e) << '\n';
        }
        std::cout << '\n';
        printBook(engine.book(), std::cout);
        std::cout << '\n';
    }

    std::cout << "commands=" << commands.size() << '\n'
              << "events=" << events.size() << '\n'
              << "event_hash=" << hashEvents(events) << '\n'
              << "book_checksum=" << engine.book().checksum() << '\n'
              << "state_checksum=" << engine.stateChecksum() << '\n';
    return 0;
}
