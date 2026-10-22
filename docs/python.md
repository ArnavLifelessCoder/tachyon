# Python bindings and backtest

`me_py` is a pybind11 module (`src/bindings/me_py.cpp`) over the same C++ types the
rest of the project uses. It is built in non-sanitizer configurations only: an
ASan- or TSan-instrumented extension cannot be loaded into an uninstrumented Python
interpreter. Build it with the debug or release preset and put the build directory
on `sys.path` (the tests and the backtest take `--module-dir`).

```python
import me_py

replay = me_py.Replay("tests/fixtures/01302019.AAPL-MSFT-SPY.itch")
replay.advance_to(9 * 3600 * 10**9)    # apply everything up to 09:00:00 exchange time
replay.bbo("AAPL")                      # (bid, bid_size, ask, ask_size), prices in 1/10000 $
replay.depth("AAPL", 5)                 # {"bids": [(price, shares, orders), ...], "asks": [...]}
replay.stats()                          # message and book counters, including unknown_refs

me_py.match([
    {"op": "new", "id": 1, "side": "sell", "price": 1000000, "qty": 100, "type": "limit"},
    {"op": "new", "id": 2, "side": "buy", "price": 1000000, "qty": 40, "type": "ioc"},
    {"op": "cancel", "id": 1},
])  # -> ack, ack, trade, cancelled, as dicts
```

`Replay` steps through a mapped raw ITCH file one message at a time, so a caller can
stop at any timestamp and query the reconstructed book. `match` runs a list of
requests through a fresh matching engine with the rules in
[matching.md](matching.md).

## Tests (`tests/python/test_bindings.py`)

- A full replay of the AAPL/MSFT/SPY fixture reproduces the manifest's per-type
  counts, zero unknown references and 45,833 live orders.
- At five timestamps for each of the three symbols, `depth` and `bbo` equal a book
  rebuilt independently in Python from `ref_parser.py`'s decoded events.
- Replaying in seven-minute steps gives the same books as one full replay.
- `match` gives the same events as `ref_matcher.py` on 20 random streams, and rejects
  malformed requests.
- The backtest runs end to end on the fixture, including its plot, in the debug CI
  jobs on Windows and Linux.

## Backtest (`python/backtest.py`)

A market-making demo of the bindings on one symbol. **It is not a trading claim.**
Every 100 ms of exchange time it advances the replay, checks whether its quotes were
filled, and requotes: it joins the best bid and offer, skews both quotes against
its inventory, and stops quoting the side that would take inventory past the limit.

The fill model is deliberately simple: a quote fills when the market's opposite best
price moves to or through it, at the quote's price, ignoring queue position, partial
fills, fees and latency. Under that model a fill only happens when the price moves
against the quote, so every fill is adversely selected and the strategy should be
expected to lose money. On the fixture it does
([`results/m7/backtest.json`](../results/m7/backtest.json),
[`results/m7/backtest.png`](../results/m7/backtest.png)): AAPL from 04:00 to the end of
the fixture at 09:30:31, 198 fills (99 buys, 99 sells), flat at the end, and
-$976.00 marked to mid. A realistic backtest would need queue-position modelling,
which is out of scope.

```sh
python python/backtest.py tests/fixtures/01302019.AAPL-MSFT-SPY.itch \
    --symbol AAPL --module-dir build/release --output results/m7
```
