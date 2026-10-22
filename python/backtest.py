"""Simple market-making backtest on a replayed ITCH book, built on the me_py bindings.

This is a demonstration of the bindings, not a trading claim. The strategy joins
the best bid and offer of one symbol, skews both quotes away from its inventory,
and stops quoting the side that would breach an inventory limit. Every
`step_ms` of exchange time it advances the replay and checks for fills.

Fill model (deliberately simple):
  - Our bid fills when the market's best ask moves to or below it, our ask when
    the best bid moves to or above it; we get `size` shares at our own price.
  - Queue position, partial fills, fees and latency are ignored.
  - Because a quote only fills when the market moves through it, every fill is
    adversely selected, so this model should be expected to lose money.

Prices stay integers (1/10000 dollars) throughout; dollars appear only in the
summary and the plot labels.

  python python/backtest.py tests/fixtures/01302019.AAPL-MSFT-SPY.itch \\
      --symbol AAPL --module-dir build/release --output results/m7
"""

import argparse
import importlib
import json
import sys
from dataclasses import dataclass, field
from pathlib import Path

TICK = 100  # one cent in ITCH price units
NS_PER_MS = 1_000_000


def parse_clock(text):
    hours, minutes, seconds = (int(x) for x in text.split(":"))
    return ((hours * 60 + minutes) * 60 + seconds) * 1_000_000_000


@dataclass
class Book:
    """The strategy's state. Cash is in price units, positive when we have sold."""

    size: int
    limit: int
    inventory: int = 0
    cash: int = 0
    bid: int | None = None
    ask: int | None = None
    fills: list = field(default_factory=list)

    def requote(self, best_bid, best_ask):
        skew = (self.inventory // self.size) * TICK  # long: quote lower, short: higher
        self.bid = best_bid - skew if self.inventory + self.size <= self.limit else None
        self.ask = best_ask - skew if self.inventory - self.size >= -self.limit else None
        if self.bid is not None and self.ask is not None and self.bid >= self.ask:
            self.bid, self.ask = self.ask - TICK, self.ask  # never cross ourselves

    def check_fills(self, now, best_bid, best_ask):
        if self.bid is not None and best_ask is not None and best_ask <= self.bid:
            self.inventory += self.size
            self.cash -= self.bid * self.size
            self.fills.append((now, "buy", self.bid))
        if self.ask is not None and best_bid is not None and best_bid >= self.ask:
            self.inventory -= self.size
            self.cash += self.ask * self.size
            self.fills.append((now, "sell", self.ask))


def run(me_py, path, symbol, start_ns, end_ns, step_ns, size, limit):
    replay = me_py.Replay(str(path))
    replay.advance_to(start_ns)
    book = Book(size=size, limit=limit)
    series = []  # (time ns, mid*2, inventory, pnl*2) in integer units
    now = start_ns
    while now < end_ns and not replay.finished:
        now += step_ns
        replay.advance_to(now)
        best_bid, _, best_ask, _ = replay.bbo(symbol)
        book.check_fills(now, best_bid, best_ask)
        if best_bid is None or best_ask is None or best_bid >= best_ask:
            book.bid = book.ask = None  # one-sided or locked/crossed: stand aside
            continue
        mid2 = best_bid + best_ask
        book.requote(best_bid, best_ask)
        series.append((now, mid2, book.inventory, 2 * book.cash + book.inventory * mid2))
    return replay, book, series


def summarize(replay, book, series, symbol, step_ms, size, limit):
    pnl2 = series[-1][3] if series else 0
    return {
        "note": "demo of the me_py bindings, not a trading claim (see the fill model)",
        "symbol": symbol,
        "step_ms": step_ms,
        "quote_size": size,
        "inventory_limit": limit,
        "messages_replayed": replay.stats()["messages"],
        "unknown_refs": replay.stats()["unknown_refs"],
        "samples": len(series),
        "fills": len(book.fills),
        "buys": sum(1 for f in book.fills if f[1] == "buy"),
        "sells": sum(1 for f in book.fills if f[1] == "sell"),
        "final_inventory": book.inventory,
        "final_pnl_dollars_marked_to_mid": pnl2 / 2 / 10_000,
    }


def plot(series, fills, symbol, output):
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    hours = [t / 3.6e12 for t, *_ in series]
    mids = [m / 2 / 10_000 for _, m, _, _ in series]
    fig, (ax1, ax2, ax3) = plt.subplots(3, 1, figsize=(8, 8), sharex=True)
    ax1.plot(hours, mids, linewidth=0.8, color="gray", label="mid")
    for side, color in (("buy", "tab:green"), ("sell", "tab:red")):
        points = [(t / 3.6e12, p / 10_000) for t, s, p in fills if s == side]
        if points:
            ax1.scatter(*zip(*points, strict=True), s=8, color=color, label=f"{side} fills")
    ax1.set_ylabel(f"{symbol} price ($)")
    ax1.legend(fontsize=8)
    ax2.step(hours, [i for _, _, i, _ in series], where="post")
    ax2.set_ylabel("inventory (shares)")
    ax3.plot(hours, [p / 2 / 10_000 for *_, p in series])
    ax3.set_ylabel("PnL marked to mid ($)")
    ax3.set_xlabel("exchange time (hours since midnight)")
    for ax in (ax1, ax2, ax3):
        ax.grid(True, alpha=0.3)
    fig.suptitle(f"Market-making demo on {symbol} (not a trading claim)")
    fig.tight_layout()
    fig.savefig(output / "backtest.png", dpi=120)
    plt.close(fig)


def main(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawTextHelpFormatter
    )
    parser.add_argument("itch", type=Path, help="raw ITCH file")
    parser.add_argument("--symbol", default="AAPL")
    parser.add_argument("--module-dir", type=Path, help="directory containing the built me_py")
    parser.add_argument("--start", default="04:00:00", help="exchange time HH:MM:SS")
    parser.add_argument("--end", default="23:59:59")
    parser.add_argument("--step-ms", type=int, default=100)
    parser.add_argument("--size", type=int, default=100)
    parser.add_argument("--limit", type=int, default=500)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--no-plots", action="store_true")
    args = parser.parse_args(argv)
    if args.module_dir:
        sys.path.insert(0, str(args.module_dir.resolve()))
    me_py = importlib.import_module("me_py")
    replay, book, series = run(
        me_py,
        args.itch,
        args.symbol,
        parse_clock(args.start),
        parse_clock(args.end),
        args.step_ms * NS_PER_MS,
        args.size,
        args.limit,
    )
    summary = summarize(replay, book, series, args.symbol, args.step_ms, args.size, args.limit)
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "backtest.json").write_text(json.dumps(summary, indent=2) + "\n")
    if not args.no_plots:
        plot(series, book.fills, args.symbol, args.output)
    print(json.dumps(summary, indent=2))
    return summary


if __name__ == "__main__":
    main()
