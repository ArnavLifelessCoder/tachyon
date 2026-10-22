# Matching engine

The book builder (M3) reconstructs a book the exchange already matched. The matching
engine (`include/me/matching_engine.hpp`) is the other half: it accepts orders and
decides the trades itself, with price-time priority. It reuses the book's structures:
pooled `Order`s, intrusive `Level` FIFOs, the sorted level vectors and the
open-addressing id map. One engine handles one symbol.

## Interface

| Input | Fields |
|---|---|
| `NewOrder` | id, side, price, qty, type (limit, market, IOC) |
| `CancelOrder` | id |
| `ModifyOrder` | id, new price, new open quantity |

| Event | Meaning |
|---|---|
| `Ack` | request accepted (echoes the request's price and quantity) |
| `Trade` | taker id, maker id, maker's price, quantity |
| `Cancelled` | quantity removed (explicit cancel, or IOC/market remainder) |
| `Rejected` | reason: `invalid_quantity`, `invalid_price`, `duplicate_id`, `unknown_order`, `no_liquidity` |

Events go to a caller-supplied callable in the order they happen. Prices are the
same integer ticks used everywhere else.

## Rules

- **Price-time priority.** An incoming order trades against the best opposite price
  first and, within a price, the oldest order first. Every trade executes at the
  resting (maker) order's price.
- **Limit**: trades while it crosses; any remainder rests at its limit.
- **IOC**: trades while it crosses; any remainder is cancelled, never rests.
- **Market**: trades at any price until filled or the opposite side is empty; the
  remainder is cancelled. A market order with an empty opposite side is rejected
  (`no_liquidity`) with no ack.
- **Event order for a new order**: `Ack`, then each `Trade`, then `Cancelled` for an
  IOC or market remainder.
- **Modify** sets a new price and a new open quantity.
  - Same price and a lower (or equal) quantity: reduced in place, **time priority
    kept**. A trader shrinking an order has not asked for anything new.
  - A price change or a quantity increase: treated as cancel plus re-add. The order
    **loses priority**, goes to the back of its new level, and can trade
    immediately if the new price crosses (it re-enters as a limit order).
  - A successful modify emits `Ack`, then any trades from the re-add.
- **Rejected**: quantity 0; price 0 on a limit or IOC order (or a modify); a new
  order whose id is already resting; cancel or modify of an id that is not resting.
  A filled or cancelled id may be reused.
- **Out of scope**: self-trade prevention, multiple symbols per engine, order
  expiry, and risk checks.

Resting, cancelling and filling never allocate once the pools are warm;
`book_allocations` checks this by replaying 200,000 orders after a warm-up pass.

## Tests

- **Scenarios** (`tests/matching_test.cpp`): partial fills, the taker remainder
  resting, walking several levels best first, time priority within a level, not
  trading through a limit, IOC remainder cancelled, IOC with nothing to match, market
  order on an empty book rejected, market order walking any price, cancel, the three
  modify priority rules, modify that re-enters and trades, every reject reason, and id
  reuse.
- **Differential** (`tests/python/test_matcher.py`): `python/ref_matcher.py` is a
  deliberately simple matcher (one flat list of resting orders, re-sorted to find the
  best one every time). 1,000 random streams, each starting from an empty book, mix
  limit, IOC and market orders, cancels, modifies and invalid requests. All streams
  go through the C++ engine in one `match_orders` process and through the Python
  matcher, and every event sequence must be identical (acks, trades, cancels and
  rejects, not only trades). The test also requires every event type and every
  reject reason to appear, so the generator cannot silently stop covering them.
  Rerun with `pytest tests/python/test_matcher.py --parser <build>/itch_parse --seed=N`.
- `match_orders` speaks a small text protocol, documented in
  `include/me/order_protocol.hpp`, so both implementations read the same input.
