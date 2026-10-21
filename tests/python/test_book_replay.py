import json
import subprocess
from pathlib import Path

FIXTURES = Path(__file__).resolve().parents[1] / "fixtures"


def replay(parser_executable, source):
    executable = parser_executable.with_name("book_replay" + parser_executable.suffix)
    return subprocess.run(
        [str(executable), str(source)], capture_output=True, text=True, timeout=60
    )


def test_fixture_replay_reports_manifest_counts(parser_executable):
    manifest = json.loads((FIXTURES / "01302019.AAPL-MSFT-SPY.manifest.json").read_text())
    result = replay(parser_executable, FIXTURES / manifest["fixture_file"])
    assert result.returncode == 0, result.stderr
    data = json.loads(result.stdout)
    counts = manifest["counts"]
    assert data["messages"] == manifest["messages"]
    assert data["counts"] == counts
    assert data["adds"] == counts["A"] + counts["F"]
    assert data["executes"] == counts["E"] + counts["C"]
    assert data["cancels"] == counts["X"]
    assert data["deletes"] == counts["D"]
    assert data["replaces"] == counts["U"]
    assert data["unknown_refs"] == 0
    assert data["live_orders_at_end"] == 45833  # independently counted in Python during M2
    assert data["books"] == 3


def test_unknown_reference_sets_failure_status(parser_executable, tmp_path):
    from python.itch_writer import encode_events

    source = tmp_path / "unknown.itch"
    source.write_bytes(encode_events([{"type": "D", "stock_locate": 1, "order_id": 42}]))
    result = replay(parser_executable, source)
    assert result.returncode == 3
    assert json.loads(result.stdout)["unknown_refs"] == 1
