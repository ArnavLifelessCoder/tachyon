import hashlib
import json
import subprocess
import sys
from collections import Counter
from pathlib import Path

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


def test_real_fixture_matches_manifest():
    """Keep the real prefix mandatory for the parser milestone's count oracle."""
    manifest = json.loads(
        (FIXTURES / "01302019.first100000.manifest.json").read_text(encoding="utf-8")
    )
    fixture = (FIXTURES / manifest["fixture_file"]).read_bytes()
    assert manifest["source_file"] == "01302019.NASDAQ_ITCH50.gz"
    assert manifest["synthetic"] is False
    assert len(fixture) == manifest["bytes"] < 5_000_000
    assert hashlib.sha256(fixture).hexdigest() == manifest["sha256"]
    counts = Counter()
    offset = 0
    while offset < len(fixture):
        assert offset + 2 <= len(fixture)
        length = int.from_bytes(fixture[offset : offset + 2], "big")
        assert length > 0
        assert offset + 2 + length <= len(fixture)
        counts[chr(fixture[offset + 2])] += 1
        offset += 2 + length
    assert sum(counts.values()) == manifest["messages"] == 100_000
    assert dict(sorted(counts.items())) == manifest["counts"]
