import json

import pytest

from python.itch_writer import encode_event
from python.parse_file import run_parser_batch
from python.ref_parser import parse


def test_batch_preserves_order_and_continues_after_errors(parser_executable, tmp_path, monkeypatch):
    monkeypatch.setenv("ITCH_PARSE_TRACE", "1")
    good = tmp_path / "行情 seed-17.itch"
    bad = tmp_path / "seed-18.itch"
    empty = tmp_path / "seed-19.itch"
    missing = tmp_path / "seed-20.itch"
    good.write_bytes(encode_event({"type": "S", "event_code": "O"}))
    bad.write_bytes(b"\x00")
    empty.write_bytes(b"")
    inputs = [good, bad, missing, empty, good]
    result = run_parser_batch(inputs, parser_executable, events=True)
    assert result.returncode == 0, result.stderr
    rows = [json.loads(line) for line in result.stdout.splitlines()]
    assert len(rows) == len(inputs)
    assert rows[0] == {"status": 0, "result": parse(good.read_bytes(), events=True)}
    assert rows[1] == {"status": 2, "result": {"error": "truncated_prefix", "offset": 0}}
    assert rows[2]["status"] == 1
    assert rows[2]["result"]["error"] == "io_error"
    assert rows[3] == {"status": 0, "result": parse(b"", events=True)}
    assert rows[4] == rows[0]
    assert "itch_parse: main entered" in result.stderr
    assert "itch_parse: main returning status=0" in result.stderr


def test_batch_empty_list(parser_executable):
    result = run_parser_batch([], parser_executable)
    assert result.returncode == 0, result.stderr
    assert result.stdout == ""


def test_batch_counts_without_events(parser_executable, tmp_path):
    source = tmp_path / "seed-21.itch"
    source.write_bytes(encode_event({"type": "S", "event_code": "C"}))
    result = run_parser_batch([source, source], parser_executable)
    assert result.returncode == 0, result.stderr
    assert [json.loads(line) for line in result.stdout.splitlines()] == [
        {"status": 0, "result": parse(source.read_bytes())}
    ] * 2


@pytest.mark.parametrize("trace", [None, "0", "1", "10"])
def test_main_markers_require_trace_one(parser_executable, monkeypatch, trace):
    monkeypatch.delenv("ME_TRACE", raising=False)
    if trace is None:
        monkeypatch.delenv("ITCH_PARSE_TRACE", raising=False)
    else:
        monkeypatch.setenv("ITCH_PARSE_TRACE", trace)
    result = run_parser_batch([], parser_executable)
    assert result.returncode == 0, result.stderr
    assert result.stdout == ""
    assert ("itch_parse: main entered" in result.stderr) == (trace == "1")
    assert ("itch_parse: main returning status=0" in result.stderr) == (trace == "1")


def test_main_markers_accept_me_trace(parser_executable, monkeypatch):
    monkeypatch.delenv("ITCH_PARSE_TRACE", raising=False)
    monkeypatch.setenv("ME_TRACE", "1")
    result = run_parser_batch([], parser_executable)
    assert result.returncode == 0
    assert "itch_parse: main entered" in result.stderr
    assert "itch_parse: main returning status=0" in result.stderr
