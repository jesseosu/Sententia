# Domain Model

The engine's entire interface is two types: a `Command` going in and an
`Event` coming out. Everything else in this document is detail about
those two.

```
Command  ---->  MatchingEngine::apply  ---->  [Event]
                       |
                       v
                  OrderBook state
```

## Value types

| Type | Underlying | Notes |
|------|-----------|-------|
| `OrderId` | `uint64_t` | Unique among *live* orders only. See below. |
| `InstrumentId` | `uint32_t` | One engine instance handles one instrument. |
| `Price` | `int64_t` | Integer ticks. Never floating point. |
| `Quantity` | `uint64_t` | Whole units. |
| `Sequence` | `uint64_t` | Logical time. The engine's only clock. |

Prices are integer ticks because floating point is deterministic on a
fixed platform and toolchain but not portably so across them. Phase 3
replication requires two different machines to compute bit-identical
results, so floating point is ruled out from the start rather than
discovered to be a problem later.

## Commands

A command is an *intent* submitted from outside. It carries nothing the
engine decides: no timestamps, no sequence numbers, no derived fields.
That restriction is what makes a command replayable. If a command
carried a timestamp taken from the submitter's clock, two replicas
replaying the same log would still be replaying the same bytes, but the
moment the engine used that timestamp for anything the log would stop
being a complete description of the input.

```cpp
struct NewOrder {
    OrderId     id;
    InstrumentId instrument;
    Side        side;         // Buy | Sell
    OrderType   type;         // Limit | Market
    TimeInForce tif;          // GoodTillCancel | ImmediateOrCancel
    Price       price;        // ignored for market orders
    Quantity    quantity;
};

struct CancelOrder {
    OrderId id;
};

using Command = std::variant<NewOrder, CancelOrder>;
```

## Events

An event is a *fact*: something already decided. Events are the engine's
only output, and the book state is fully derivable by replaying them,
which is what lets the event log serve as the unit of replication in
Phase 3 and the unit of recovery in Phase 5.

| Event | Emitted when |
|-------|--------------|
| `OrderAccepted` | A new order passes validation. Always first for that command. |
| `OrderRejected` | A new order fails validation. The only event for that command. |
| `Trade` | Quantity crosses. One event per resting order consumed. |
| `OrderResting` | Residual quantity comes to rest on the book. |
| `OrderCancelled` | A client cancel succeeds, or the engine cancels a residual. |
| `CancelRejected` | A cancel names an order that is not live. |
| `TopOfBookChanged` | The best bid or best ask changed. At most one per command. |

Every event carries an `EventHeader` with two numbers: `commandSeq`,
identifying the command that caused it, and `eventSeq`, its own ordinal.
Event sequence numbers are gapless and strictly increasing across the
engine's whole lifetime, which is asserted in `test_invariants`.

### Event ordering within one command

The order is fixed and is part of the contract. Changing it changes the
observable behaviour of the engine even though no individual event
changes:

1. `OrderAccepted` or `OrderRejected`
2. `Trade`, one per resting order consumed, in match order
3. `OrderResting` or `OrderCancelled` for any residual
4. `TopOfBookChanged`, if the top actually moved

`TopOfBookChanged` comes last because it describes the state *after* the
command, and it is suppressed when the top is unchanged. A cancel deep in
the book therefore emits no top-of-book event at all.

## Matching rules

**Price priority, then time priority, and nothing else.** Not quantity,
not order id, not the submitter.

- Price priority: the best price on the opposite side matches first.
  Highest bid, lowest ask.
- Time priority: within one price level, the earliest arrival matches
  first, tracked by an engine-assigned arrival ordinal rather than by any
  clock.
- Execution price is always the *resting* order's price. The resting
  order set the terms and the aggressor accepted them, so any price
  improvement accrues to the aggressor.
- A partially filled resting order keeps its place at the front of its
  level. It does not go to the back.
- Cancelling and re-entering loses priority. This is why the tie-break is
  an arrival ordinal and not the order id.

## Order lifecycle

```
NewOrder
   |
   +-- validation fails ------------------> OrderRejected  [terminal]
   |
   +-- OrderAccepted
         |
         +-- cross against opposite book --> Trade (0..n)
               |
               +-- residual == 0 ----------------------> [terminal, filled]
               |
               +-- residual > 0, limit + GTC ---------> OrderResting
               |                                            |
               |                                            +-- later Trade(s) -> filled
               |                                            +-- later CancelOrder -> OrderCancelled
               |
               +-- residual > 0, IOC or market -------> OrderCancelled (engine)
```

## Time in force

`GoodTillCancel` orders rest until filled or cancelled.
`ImmediateOrCancel` orders match what is available now, and the engine
cancels the residual. IOC orders never appear in the book.

Market orders cannot rest by definition, so they must be submitted as
IOC. A market order marked GTC is *rejected* rather than silently
reinterpreted, because quietly rewriting a caller's stated intent is the
kind of behaviour that becomes very hard to reason about once several
nodes are involved.

## Reject reasons

Validation runs in a fixed order, and the order is part of the contract:
a command that violates several rules always reports the same one.

1. `WrongInstrument` - the command is for a different instrument.
2. `ZeroQuantity` - quantity is zero.
3. `NonPositivePrice` - a limit order priced at or below zero.
4. `MarketOrderMustBeIoc` - a market order not marked IOC.
5. `DuplicateOrderId` - an order with this id is already live.
6. `UnknownOrderId` - a cancel naming an order that is not live.

Rejected commands still consume a command sequence number. The log
records what was *submitted*, not only what worked, so a replica can
replay it verbatim and reach the same conclusion, rejections included.

## Order id semantics

Ids are unique among *live* orders, not for all time. Once an order is
fully filled or cancelled its id is free to be reused. This keeps the
engine's memory bounded by the size of the book rather than by the
history of every order ever submitted, which matters once a node has to
be able to rebuild its state from a log.

## Deliberate omissions

Stated explicitly so they read as decisions rather than oversights.

- **No participant identity.** There are no accounts, so self-matching is
  permitted; there is nobody to protect from trading with themselves.
- **No fees, no position keeping, no risk checks.** Out of scope. This is
  a matching engine, not a clearing system.
- **One instrument per engine.** Multi-instrument support would be a map
  of books, which adds no distributed-systems interest. The
  `WrongInstrument` reject exists so the boundary is enforced rather than
  assumed.
- **No modify or replace command.** Cancel then new order expresses the
  same thing with fewer states to reason about, and matches how priority
  actually works.
