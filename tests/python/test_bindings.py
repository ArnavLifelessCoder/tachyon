import json
from pathlib import Path

import pytest

from python import backtest
from python.ref_matcher import run as ref_run
from python.ref_parser import parse

FIXTURES = Path(__file__).resolve().parents[1] / "fixtures"
FIXTURE = FIXTURES / "01302019.AAPL-MSFT-SPY.itch"
MANIFEST = json.loads((FIXTURES / "01302019.AAPL-MSFT-SPY.manifest.json").read_text())


def clock(text):
    return backtest.parse_clock(text)


def test_full_replay_matches_manifest(me_py):
    replay = me_py.Replay(str(FIXTURE))
    replay.run_to_end()
    stats, counts = replay.stats(), MANIFEST["counts"]
    assert replay.finished
    assert stats["messages"] == MANIFEST["messages"]
    assert stats["adds"] == counts["A"] + counts["F"]
    assert stats["executes"] == counts["E"] + counts["C"]
    assert stats["cancels"] == counts["X"]
    assert stats["deletes"] == counts["D"]
    assert stats["replaces"] == counts["U"]
    assert stats["unknown_refs"] == 0
    assert stats["live_orders"] == 45833  # counted independently when the fixture was cut
    assert replay.locate("AAPL") == 14
    assert replay.locate("NOPE") is None
    assert replay.bbo("NOPE") == (None, None, None, None)


def reference_depth(events, locate, timestamp, levels=5):
    """Independent book from ref_parser events: id -> [side, price, qty]."""
    orders = {}
    for e in events:
        if e.get("timestamp", 0) > timestamp:
            break
        kind = e["type"]
        if kind in "AF" and e["stock_locate"] == locate:
            orders[e["order_id"]] = [e["side"], e["price"], e["shares"]]
        elif kind in "ECX" and e["order_id"] in orders:
            orders[e["order_id"]][2] -= e["shares"]
            if orders[e["order_id"]][2] <= 0:
                del orders[e["order_id"]]
        elif kind == "D":
            orders.pop(e["order_id"], None)
        elif kind == "U" and e["order_id"] in orders:
            side = orders.pop(e["order_id"])[0]
            orders[e["new_order_id"]] = [side, e["price"], e["shares"]]
    out = {}
    for side, key, name in (("B", lambda p: -p, "bids"), ("S", lambda p: p, "asks")):
        levels_map = {}
        for s, price, qty in orders.values():
            if s == side:
                total, count = levels_map.get(price, (0, 0))
                levels_map[price] = (total + qty, count + 1)
        best = sorted(levels_map, key=key)[:levels]
        out[name] = [(p, levels_map[p][0], levels_map[p][1]) for p in best]
    return out


@pytest.fixture(scope="module")
def events():
    return parse(FIXTURE.read_bytes(), events=True)["events"]


@pytest.mark.parametrize("time", ["04:00:00", "07:00:00", "09:00:00", "09:30:00", "23:59:59"])
@pytest.mark.parametrize("symbol", ["AAPL", "MSFT", "SPY"])
def test_depth_and_bbo_match_independent_python_book(me_py, events, time, symbol):
    replay = me_py.Replay(str(FIXTURE))
    replay.advance_to(clock(time))
    assert replay.now <= clock(time)
    want = reference_depth(events, replay.locate(symbol), clock(time))
    got = replay.depth(symbol, 5)
    assert [tuple(x) for x in got["bids"]] == want["bids"]
    assert [tuple(x) for x in got["asks"]] == want["asks"]
    bid, bid_size, ask, ask_size = replay.bbo(symbol)
    assert (bid, bid_size) == (want["bids"][0][:2] if want["bids"] else (None, None))
    assert (ask, ask_size) == (want["asks"][0][:2] if want["asks"] else (None, None))


def test_stepwise_replay_equals_one_shot(me_py):
    whole = me_py.Replay(str(FIXTURE))
    whole.run_to_end()
    steps = me_py.Replay(str(FIXTURE))
    applied, now = 0, 0
    while not steps.finished:
        now += 7 * 60 * 1_000_000_000  # seven-minute steps
        applied += steps.advance_to(now)
    assert applied == whole.stats()["messages"]
    for symbol in ("AAPL", "MSFT", "SPY"):
        assert steps.depth(symbol, 10) == whole.depth(symbol, 10)


def to_requests(lines):
    """Protocol lines (see order_protocol.hpp) to me_py.match request dicts."""
    sides, types = {"B": "buy", "S": "sell"}, {"L": "limit", "M": "market", "I": "ioc"}
    out = []
    for line in lines:
        f = line.split()
        if f[0] == "N":
            out.append(
                {
                    "op": "new",
                    "id": int(f[1]),
                    "side": sides[f[2]],
                    "price": int(f[3]),
                    "qty": int(f[4]),
                    "type": types[f[5]],
                }
            )
        elif f[0] == "C":
            out.append({"op": "cancel", "id": int(f[1])})
        else:
            out.append({"op": "modify", "id": int(f[1]), "price": int(f[2]), "qty": int(f[3])})
    return out


def to_lines(events):
    out = []
    for e in events:
        if e["type"] == "ack":
            out.append(f"A {e['id']} {e['price']} {e['qty']}")
        elif e["type"] == "trade":
            out.append(f"T {e['id']} {e['maker_id']} {e['price']} {e['qty']}")
        elif e["type"] == "cancelled":
            out.append(f"X {e['id']} {e['price']} {e['qty']}")
        else:
            out.append(f"R {e['id']} {e['reason']}")
    return out


@pytest.mark.parametrize("seed", range(20))
def test_match_agrees_with_reference_matcher(me_py, seed):
    from tests.python.test_matcher import stream

    lines = stream(seed)[1:]  # drop the stream-start marker
    assert to_lines(me_py.match(to_requests(lines))) == ref_run(lines)


def test_match_rejects_bad_requests(me_py):
    with pytest.raises(ValueError):
        me_py.match([{"op": "new", "id": 1, "side": "up", "price": 1, "qty": 1}])
    with pytest.raises(ValueError):
        me_py.match([{"op": "amend", "id": 1}])
    events = me_py.match([{"op": "new", "id": 1, "side": "buy", "qty": 5, "type": "market"}])
    assert events == [{"type": "rejected", "id": 1, "reason": "no_liquidity"}]


def test_backtest_runs_end_to_end_on_fixture(me_py, tmp_path):
    summary = backtest.main([str(FIXTURE), "--symbol", "AAPL", "--output", str(tmp_path)])
    assert summary["unknown_refs"] == 0
    assert summary["samples"] > 1000
    assert summary["fills"] == summary["buys"] + summary["sells"]
    assert abs(summary["final_inventory"]) <= summary["inventory_limit"]
    assert json.loads((tmp_path / "backtest.json").read_text()) == summary
    assert (tmp_path / "backtest.png").stat().st_size > 10_000
