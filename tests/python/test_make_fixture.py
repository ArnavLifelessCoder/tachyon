import gzip
import json
import subprocess
import sys
from pathlib import Path

import pytest

from python.itch_writer import encode_event, frame
from scripts.make_fixture import filter_fixture, make_fixture

STREAM = b"\x00\x03Sxy\x00\x04?abc\x00\x01D\x00\x00"


@pytest.mark.parametrize("compressed", [False, True])
def test_exact_prefix_raw_and_gzip(tmp_path, compressed):
    source = tmp_path / ("source.gz" if compressed else "source.itch")
    original = gzip.compress(STREAM) if compressed else STREAM
    source.write_bytes(original)
    output = tmp_path / "fixture.itch"
    assert make_fixture(source, output, 2) == 11
    assert output.read_bytes() == STREAM[:11]
    assert source.read_bytes() == original


@pytest.mark.parametrize("data", [b"", b"\0", b"\0\x03S", b"\0\0"])
def test_reject_truncation_and_early_session_end(tmp_path, data):
    source, output = tmp_path / "input", tmp_path / "output"
    source.write_bytes(data)
    with pytest.raises(EOFError):
        make_fixture(source, output, 1)
    assert not output.exists()
    assert source.read_bytes() == data


def test_refuse_overwrite_and_same_file(tmp_path):
    source, output = tmp_path / "input", tmp_path / "output"
    source.write_bytes(STREAM)
    output.write_bytes(b"keep")
    with pytest.raises(FileExistsError):
        make_fixture(source, output, 1)
    with pytest.raises(FileExistsError):
        make_fixture(source, source, 1)
    assert output.read_bytes() == b"keep"
    assert source.read_bytes() == STREAM


def test_size_cap_removes_partial_output(tmp_path, monkeypatch):
    source, output = tmp_path / "input", tmp_path / "output"
    source.write_bytes(STREAM)
    monkeypatch.setattr("scripts.make_fixture.MAX_FIXTURE_BYTES", 11)
    with pytest.raises(ValueError, match="smaller"):
        make_fixture(source, output, 2)
    assert not output.exists()


def test_zero_messages_and_unread_truncated_tail(tmp_path):
    source = tmp_path / "input"
    source.write_bytes(STREAM[:5] + b"\0")
    empty, first = tmp_path / "empty", tmp_path / "first"
    assert make_fixture(source, empty, 0) == 0
    assert empty.read_bytes() == b""
    assert make_fixture(source, first, 1) == 5
    assert first.read_bytes() == STREAM[:5]


@pytest.mark.parametrize("count", [-1, True, 1.5])
def test_invalid_count(tmp_path, count):
    with pytest.raises(ValueError, match="count"):
        make_fixture(tmp_path / "missing", tmp_path / "output", count)


def test_corrupt_gzip_removes_output(tmp_path):
    source, output = tmp_path / "input.gz", tmp_path / "output"
    source.write_bytes(b"not gzip")
    with pytest.raises(gzip.BadGzipFile):
        make_fixture(source, output, 1)
    assert not output.exists()


def filtered_frames():
    return [
        encode_event({"type": "S", "event_code": "O"}),
        encode_event({"type": "R", "stock_locate": 7, "stock": "ABC"}),
        encode_event({"type": "R", "stock_locate": 8, "stock": "XYZ"}),
        encode_event({"type": "D", "stock_locate": 8, "order_id": 1}),
        encode_event({"type": "D", "stock_locate": 7, "order_id": 2}),
        frame(b"?" + b"\0\x07" + bytes(8) + b"unknown"),
        encode_event({"type": "S", "event_code": "C"}),
    ]


@pytest.mark.parametrize("compressed", [False, True])
@pytest.mark.parametrize("terminator", [b"", b"\0\0"])
def test_filter_preserves_global_and_selected_messages(tmp_path, compressed, terminator):
    frames = filtered_frames()
    source = tmp_path / ("source.gz" if compressed else "source.itch")
    original = b"".join(frames) + terminator
    source.write_bytes(gzip.compress(original) if compressed else original)
    output = tmp_path / "output"
    stats = filter_fixture(source, output, {7})
    assert output.read_bytes() == b"".join(frames[:3] + frames[4:])
    assert stats == {
        "messages": 6,
        "bytes": len(output.read_bytes()),
        "counts": {"?": 1, "D": 1, "R": 2, "S": 2},
        "locates": [7],
        "symbols": {"7": "ABC"},
        "source_messages_consumed": 7,
        "stop_reason": "session_end" if terminator else "eof",
    }


@pytest.mark.parametrize("extra", [0, 1])
def test_filter_stops_before_first_frame_at_limit(tmp_path, monkeypatch, extra):
    frames = filtered_frames()
    source, output = tmp_path / "source", tmp_path / "output"
    source.write_bytes(b"".join(frames))
    # The unselected D does not consume capacity. A later S must not be copied
    # after the first selected frame that no longer fits.
    limit = sum(map(len, frames[:3] + [frames[4]])) + extra
    monkeypatch.setattr("scripts.make_fixture.MAX_FIXTURE_BYTES", limit)
    stats = filter_fixture(source, output, {7})
    expected = frames[:3] + ([frames[4]] if extra else [])
    assert output.read_bytes() == b"".join(expected)
    assert stats["bytes"] < limit
    assert stats["stop_reason"] == "size_limit"


@pytest.mark.parametrize("tail", [b"\0", b"\0\x20short", frame(b"?"), frame(b"R" + bytes(10))])
def test_filter_rejects_malformed_frames_and_removes_output(tmp_path, tail):
    source, output = tmp_path / "source", tmp_path / "output"
    source.write_bytes(filtered_frames()[0] + tail)
    with pytest.raises((EOFError, ValueError)):
        filter_fixture(source, output, {7})
    assert not output.exists()


@pytest.mark.parametrize("locates", [set(), {-1}, {65536}, {True}, {1.5}])
def test_filter_invalid_locates(tmp_path, locates):
    with pytest.raises(ValueError, match="locates"):
        filter_fixture(tmp_path / "missing", tmp_path / "output", locates)


def test_filter_refuses_existing_output(tmp_path):
    source, output = tmp_path / "source", tmp_path / "output"
    source.write_bytes(b"".join(filtered_frames()))
    output.write_bytes(b"keep")
    with pytest.raises(FileExistsError):
        filter_fixture(source, output, {7})
    with pytest.raises(FileExistsError):
        filter_fixture(source, source, {7})
    assert output.read_bytes() == b"keep"


def test_filter_command_line_multiple_locates(tmp_path):
    frames = filtered_frames()
    source, output = tmp_path / "source", tmp_path / "output"
    source.write_bytes(b"".join(frames))
    result = subprocess.run(
        [
            sys.executable,
            str(Path(__file__).resolve().parents[2] / "scripts" / "make_fixture.py"),
            str(source),
            str(output),
            "--locates",
            "7",
            "8",
        ],
        check=True,
        capture_output=True,
        text=True,
        timeout=20,
    )
    assert output.read_bytes() == b"".join(frames)
    assert json.loads(result.stdout)["symbols"] == {"7": "ABC", "8": "XYZ"}
