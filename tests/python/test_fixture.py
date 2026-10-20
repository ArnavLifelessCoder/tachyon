import hashlib
import json
import subprocess
import sys
from collections import Counter
from pathlib import Path

import pytest

from python.itch_writer import encode_events

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "tests" / "fixtures"


def test_committed_fixture_is_reproducible():
    events = json.loads((FIXTURES / "synthetic.json").read_text(encoding="utf-8"))
    fixture = (FIXTURES / "synthetic.itch").read_bytes()
    assert fixture == encode_events(events, end_session=True)
    assert len(fixture) < 5_000_000
    counts = Counter()
    offset = 0
    lengths = {
        "S": 12,
        "R": 39,
        "A": 36,
        "F": 40,
        "E": 31,
        "C": 36,
        "X": 23,
        "U": 35,
        "D": 19,
        "P": 44,
    }
    while offset < len(fixture) - 2:
        length = int.from_bytes(fixture[offset : offset + 2], "big")
        kind = chr(fixture[offset + 2])
        assert length == lengths[kind]
        counts[kind] += 1
        offset += 2 + length
    assert fixture[offset:] == b"\0\0"
    assert counts == {
        "S": 2,
        "R": 2,
        "A": 2,
        "F": 1,
        "E": 2,
        "C": 1,
        "X": 1,
        "U": 1,
        "D": 2,
        "P": 1,
    }


def test_writer_and_cutter_command_lines(tmp_path):
    generated, cut = tmp_path / "synthetic.itch", tmp_path / "cut.itch"
    subprocess.run(
        [
            sys.executable,
            str(ROOT / "python" / "itch_writer.py"),
            str(FIXTURES / "synthetic.json"),
            str(generated),
            "--end-session",
        ],
        check=True,
        capture_output=True,
        text=True,
        timeout=20,
    )
    assert generated.read_bytes() == (FIXTURES / "synthetic.itch").read_bytes()
    subprocess.run(
        [
            sys.executable,
            str(ROOT / "scripts" / "make_fixture.py"),
            str(generated),
            str(cut),
            "--messages",
            "2",
        ],
        check=True,
        capture_output=True,
        text=True,
        timeout=20,
    )
    assert cut.read_bytes() == generated.read_bytes()[:55]


def test_download_command_line_help():
    result = subprocess.run(
        [sys.executable, str(ROOT / "scripts" / "download_itch.py"), "--help"],
        check=True,
        capture_output=True,
        text=True,
        timeout=20,
    )
    assert "--expected-size" in result.stdout


@pytest.mark.parametrize("name", ["01302019.first100000", "01302019.AAPL-MSFT-SPY"])
def test_real_fixture_matches_manifest(name):
    manifest = json.loads((FIXTURES / f"{name}.manifest.json").read_text(encoding="utf-8"))
    fixture = (FIXTURES / manifest["fixture_file"]).read_bytes()
    assert manifest["source_file"] == "01302019.NASDAQ_ITCH50.gz"
    assert manifest["synthetic"] is False
    assert len(fixture) == manifest["bytes"] < 5_000_000
    assert hashlib.sha256(fixture).hexdigest() == manifest["sha256"]
    counts = Counter()
    directories = {}
    offset = 0
    while offset < len(fixture):
        assert offset + 2 <= len(fixture)
        length = int.from_bytes(fixture[offset : offset + 2], "big")
        assert length > 0
        assert offset + 2 + length <= len(fixture)
        payload = fixture[offset + 2 : offset + 2 + length]
        kind = chr(payload[0])
        counts[kind] += 1
        if "locates" in manifest:
            locate = int.from_bytes(payload[1:3], "big")
            assert kind in ("S", "R") or locate in manifest["locates"]
            if kind == "R":
                directories[str(locate)] = payload[11:19].decode("ascii").rstrip()
        offset += 2 + length
    assert sum(counts.values()) == manifest["messages"]
    assert dict(sorted(counts.items())) == manifest["counts"]
    if "locates" in manifest:
        assert all(counts[kind] > 0 for kind in "SRAFECXDUP")
        assert manifest["symbols"] == {str(x): directories[str(x)] for x in manifest["locates"]}
        assert set(manifest["symbols"].values()) == {"AAPL", "MSFT", "SPY"}
        assert manifest["stop_reason"] == "size_limit"
        assert 0 < 5_000_000 - len(fixture) < 48
    else:
        assert manifest["messages"] == 100_000
