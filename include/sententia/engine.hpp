// Sententia - the deterministic matching core.
//
// This class is the whole point of Phase 1. It is a pure state machine:
//
//     apply(command) -> [events]
//
// Given the same starting state and the same ordered command sequence it
// produces the same events and the same final book, on every run, on
// every machine, forever. That property is what Phase 3 replication and
// Phase 5 recovery are built on.
//
// Things deliberately absent from this file and its implementation:
//
//   * Wall-clock time. Ordering comes from an internal counter.
//   * Randomness. Nothing here consults an RNG.
//   * Threads, atomics, locks. The core is single-threaded by contract.
//   * I/O of any kind. No sockets, no files, no logging.
//
// All of those belong at the edges (apps/, bench/), never in here.
#pragma once

#include <cstdint>
#include <vector>

#include "sententia/command.hpp"
#include "sententia/event.hpp"
#include "sententia/order_book.hpp"

namespace sententia {

// A complete, restorable picture of an engine's state.
//
// Everything needed to reconstruct an engine so that stateChecksum()
// matches, including the counters. Leaving out arrivalCounter_ would
// restore a book that looks right and breaks time priority for every
// order placed afterwards, which is the kind of bug that would surface
// weeks later as unfair fills.
//
// Orders are in canonical order: bids by descending price, asks by
// ascending, FIFO within a level. Restoring in that order reproduces the
// queues exactly.
struct EngineSnapshot {
    InstrumentId instrument{};
    Sequence commandSeq{};
    Sequence eventSeq{};
    Sequence arrivalCounter{};
    TopOfBook lastTop{};
    std::vector<RestingOrder> orders;

    friend bool operator==(const EngineSnapshot&, const EngineSnapshot&) = default;
};

class MatchingEngine {
public:
    explicit MatchingEngine(InstrumentId instrument) noexcept;

    // Applies one command, appending the resulting events to `out`.
    // `out` is not cleared, so a caller can accumulate a whole run.
    void apply(const Command& cmd, EventList& out);

    // Convenience overload. Allocates; the append form is preferred on
    // any path where allocation matters.
    EventList apply(const Command& cmd);

    const OrderBook& book() const noexcept { return book_; }
    InstrumentId instrument() const noexcept { return instrument_; }

    // Number of commands applied so far. Also the sequence number of the
    // most recently applied command.
    Sequence commandSequence() const noexcept { return commandSeq_; }
    Sequence eventSequence() const noexcept { return eventSeq_; }

    // Combines the book checksum with the engine's counters. Two engines
    // that agree on this have identical observable state.
    std::uint64_t stateChecksum() const noexcept;

    // Captures everything needed to rebuild this engine elsewhere. Pure:
    // no I/O, no clock. Serialising it is the storage layer's job.
    EngineSnapshot snapshot() const;

    // Replaces this engine's state wholesale. Returns false if the
    // snapshot is for a different instrument or is internally
    // inconsistent, rather than half-applying it.
    bool restore(const EngineSnapshot& snap);

private:
    void applyNewOrder(const NewOrder& cmd, EventList& out);
    void applyCancel(const CancelOrder& cmd, EventList& out);

    // Crosses `incoming` against the opposite book, emitting trades.
    // Returns the residual quantity that did not match.
    Quantity match(const NewOrder& cmd, Quantity quantity, EventList& out);

    EventHeader nextHeader() noexcept;
    void emitTopOfBookIfChanged(EventList& out);

    InstrumentId instrument_{};
    OrderBook book_;
    Sequence commandSeq_{0};
    Sequence eventSeq_{0};
    // Arrival ordinal for time priority. Distinct from commandSeq_ so
    // that the tie-break rule stays explicit rather than incidental.
    Sequence arrivalCounter_{0};
    TopOfBook lastTop_{};
};

// Hashes an event stream in order. Used by the determinism test to
// compare whole runs cheaply.
std::uint64_t hashEvents(const EventList& events);

}  // namespace sententia
