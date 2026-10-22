# Order book reconstruction

Replaying ITCH rebuilds the book Nasdaq published. Nothing is matched here: every
execution in the feed already happened at the exchange, and the book only removes
the shares the feed says were executed. Matching is the separate engine in M5.

## Structures

- `Order { id, price, qty, side, locate, prev, next, level }` lives in a `Pool<Order>`:
  64K-element chunks plus a free list, so addresses are stable and steady-state
  add/remove never touches the heap. `locate` is the one field beyond the spec's
  struct; it lets a reference be applied to the order's own book and lets a
  mismatched message locate be counted instead of silently trusted.
- `Level { price, count, total, head, tail }` is an intrusive FIFO of orders. Levels
  also come from a pool.
- Each side keeps `std::vector<Level*>` sorted worst to best, so the best price is at
  the back. Searches start from the back because almost all ITCH activity is at or
  near the touch. Inserting or erasing a level near the top moves a few pointers; one
  far from the touch costs O(distance). A balanced tree would bound that worst case
  but pays pointer chasing and allocation on every level change, which is the common
  case here. The benchmark in M6 measures this choice.
- `OrderMap` maps order id to `Order*`: open addressing, linear probing, power-of-two
  capacity, Fibonacci hashing, and backward-shift deletion (no tombstones, so a full
  day of deletes does not degrade probes). It is reserved up front; growth only
  happens if the reservation is exceeded.
- One `OrderBook` per stock locate in a flat 65,536-entry array, created on the first
  add for that locate. The id map is shared because ITCH ids are unique per day.

| Operation | Cost |
|---|---|
| Add at an existing level | O(1) |
| Add creating a level | O(distance from touch) |
| Execute / cancel (partial) | O(1) |
| Delete, or reduce to zero | O(1), plus O(distance) if the level empties |
| Best bid / offer | O(1) |

## Message semantics

| Type | Effect |
|---|---|
| A, F | add a resting order |
| E, C | remove executed shares; the order is removed at zero. C's price is a print price and does not move the order |
| X | remove cancelled shares |
| D | remove the order |
| U | remove the old id, add the new id with the message's price and shares, keeping the old order's side and symbol. The new order goes to the back of its level (time priority is lost) |
| P | counted, no book effect |

References to ids that are not resting are counted as `unknown_refs` and ignored.
Shares beyond what is resting are counted as `over_executions` and remove the order.

## Correctness checks

- **Differential test** (`tests/book_test.cpp`): a slow reference book
  (`tests/reference_book.hpp`, `std::map` levels of `std::list` FIFOs) and the fast
  builder process the same random ITCH-shaped streams for 10,000 seeds
  (2,254,919 events with the default base seed). After every event the top 5 levels
  on both sides of the touched book must match exactly (price, order count, total
  shares). Every 64 events and at the end of each stream the full books are compared
  and the fast book's structure is checked. Streams include partial, full and
  over-executions, cancels, deletes, replaces, F adds, duplicate ids, unknown ids,
  deep levels and crossed/locked prices across one to three symbols. Rerun one seed
  with `book_tests --gtest_filter=BookDifferential.* --seed=N --seeds=1`.
- **Invariants** (debug builds, `ME_BOOK_CHECKS`): after every book change the
  touched level is checked: non-empty, count and total equal the sum of its orders,
  no zero-quantity order, links and back-pointers consistent. Release builds compile
  the checks out. `OrderBook::valid()` checks whole books (strict price ordering on
  both sides, every level) and runs in the tests.
- **Real data**: the AAPL/MSFT/SPY fixture is replayed into both books and compared
  in full, with zero unknown references and 45,833 live orders at the end (the same
  count found independently in Python when the fixture was cut).
- **Allocations**: `book_allocation_test` replays the fixture, resets, and replays
  again; the second pass must make zero heap allocations.

## Full trading day (30 January 2019)

`python scripts/replay_day.py --replay build/release/book_replay.exe --output results/m3/full_day_replay.json`
decompresses the day once into `data/` and replays all 368,366,634 messages. Result
in [`results/m3/full_day_replay.json`](../results/m3/full_day_replay.json):

- **Unknown order references: 0.** Also zero duplicate adds, locate mismatches and
  over-executions.
- Every book is empty after the last message (0 live orders, 0 resting levels);
  the peak was 1,742,866 live orders.
- **Crossed or locked books: 663 book changes (598 crossed, 65 locked) in 8 episodes
  across 4 symbols.** Every episode began while the symbol was not in continuous
  trading according to its ITCH `H` (trading action) messages:

  | Symbol | Episodes | Trading state at start |
  |---|---|---|
  | SXTC | 10:27:49-10:32:49, 11:44:22-11:49:22 | P (paused) |
  | PHUN | 10:09:06-10:09:21, 10:10:19-10:18:58 | P (paused) |
  | DLPN | 09:46:52-09:47:07, 09:50:24-09:50:27 | P (paused) |
  | EYEN | 07:27:31-07:27:33, 07:27:52-07:29:02 | Q (quotation only) |

  During a pause (the SXTC episodes are exactly the five-minute limit-up/limit-down
  pause) or a quotation-only period, Nasdaq accepts and displays orders but does
  not execute them, so bids and offers can lock or cross until the reopening cross
  clears them. No book crossed during continuous trading, which is what a correct
  reconstruction of Nasdaq's own book should show.

The result file records the replay time (`replay_seconds`) and the machine. That
run is a correctness check, not a benchmark; M6 measures throughput properly.
