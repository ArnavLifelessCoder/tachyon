import random

import pytest

from python.parse_file import _invoke
from python.ref_matcher import run

STREAMS = 1000


def match_orders(parser_executable, lines):
    executable = parser_executable.with_name("match_orders" + parser_executable.suffix)
    data = "".join(f"{line}\n" for line in lines).encode()
    result = _invoke([str(executable)], [executable], input_bytes=data)
    assert result.returncode == 0, result.stderr
    return result.stdout.splitlines()


def stream(seed):
    """One random order stream around a mid price, including invalid requests."""
    rng = random.Random(seed)
    lines = ["S"]
    next_id = 1
    known = []  # ids that were submitted; some still rest, some do not
    for _ in range(rng.randint(30, 200)):
        r = rng.random()
        if r < 0.55 or not known:
            kind = rng.choices("LIM", weights=(75, 15, 10))[0]
            side = rng.choice("BS")
            price = rng.randint(95, 105)
            if rng.random() < 0.02:
                price = 0
            qty = rng.randint(1, 50) if rng.random() > 0.02 else 0
            order_id = next_id if rng.random() > 0.03 else rng.randint(1, next_id)  # rare duplicate
            next_id += 1
            known.append(order_id)
            lines.append(f"N {order_id} {side} {price} {qty} {kind}")
        elif r < 0.75:
            target = rng.choice(known) if rng.random() > 0.05 else next_id + 1000
            lines.append(f"C {target}")
        else:
            target = rng.choice(known) if rng.random() > 0.05 else next_id + 1000
            price = rng.randint(95, 105) if rng.random() > 0.02 else 0
            qty = rng.randint(1, 60) if rng.random() > 0.02 else 0
            lines.append(f"M {target} {price} {qty}")
    return lines


def split_streams(lines):
    streams, current = [], None
    for line in lines:
        if line == "S":
            current = []
            streams.append(current)
        else:
            current.append(line)
    return streams


def test_engine_matches_reference_on_random_streams(parser_executable, request):
    base = request.config.getoption("--seed")
    seeds = list(range(base, base + STREAMS))
    inputs = [stream(seed) for seed in seeds]
    expected = split_streams(run(line for lines in inputs for line in lines))
    actual = split_streams(match_orders(parser_executable, [line for s in inputs for line in s]))
    assert len(actual) == len(expected) == STREAMS
    for seed, want, got in zip(seeds, expected, actual, strict=True):
        if got != want:
            pairs = enumerate(zip(got, want, strict=False))  # lengths may differ
            first = next((i for i, (a, b) in pairs if a != b), min(len(got), len(want)))
            pytest.fail(f"seed={seed}: first difference at event {first}")
    events = [line for s in expected for line in s]
    # The streams must keep exercising every event kind.
    for prefix in ("A ", "T ", "X ", "R "):
        assert sum(line.startswith(prefix) for line in events) > STREAMS, prefix
    reasons = {line.split()[2] for line in events if line.startswith("R ")}
    assert reasons >= {
        "invalid_quantity",
        "invalid_price",
        "duplicate_id",
        "unknown_order",
        "no_liquidity",
    }


@pytest.mark.parametrize(
    ("lines", "events"),
    [
        (["N 1 S 100 10 L", "N 2 B 100 4 L"], ["A 1 100 10", "A 2 100 4", "T 2 1 100 4"]),
        (["N 1 S 100 10 L", "N 2 B 101 3 I"], ["A 1 100 10", "A 2 101 3", "T 2 1 100 3"]),
        (["N 1 B 0 5 M"], ["R 1 no_liquidity"]),
        (
            ["N 1 S 100 5 L", "N 2 S 100 5 L", "M 1 100 9", "N 3 B 100 6 L"],
            ["A 1 100 5", "A 2 100 5", "A 1 100 9", "A 3 100 6", "T 3 2 100 5", "T 3 1 100 1"],
        ),
    ],
)
def test_engine_and_reference_agree_on_hand_written_cases(parser_executable, lines, events):
    assert run(["S", *lines]) == ["S", *events]
    assert match_orders(parser_executable, ["S", *lines]) == ["S", *events]


def test_bad_input_is_rejected(parser_executable):
    executable = parser_executable.with_name("match_orders" + parser_executable.suffix)
    result = _invoke([str(executable)], [executable], input_bytes=b"N 1 X 100 5 L\n")
    assert result.returncode == 2
    assert "bad input at line 1" in result.stderr
