import gzip

import pytest

from scripts.make_fixture import make_fixture

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
