import hashlib
import io
from datetime import date
from pathlib import Path
from urllib.error import HTTPError

import pytest

from scripts import download_itch

DAY = date(2019, 1, 30)
BODY = b"synthetic compressed-file bytes"
BODY_LENGTH = str(len(BODY))


class Response(io.BytesIO):
    def __init__(self, data=BODY, length=BODY_LENGTH):
        super().__init__(data)
        self.headers = {} if length is None else {"Content-Length": length}


def mock_response(monkeypatch, response):
    def open_url(url, *, timeout):
        assert url == "https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/01302019.NASDAQ_ITCH50.gz"
        assert timeout == 60
        return response

    monkeypatch.setattr(download_itch, "urlopen", open_url)


def test_streams_checks_size_and_hash(tmp_path, monkeypatch):
    mock_response(monkeypatch, Response())
    monkeypatch.setattr(download_itch, "CHUNK_SIZE", 3)
    destination = tmp_path / "data" / "day.gz"
    result = download_itch.download(DAY, destination)
    assert destination.read_bytes() == BODY
    assert result["bytes"] == len(BODY)
    assert result["sha256"] == hashlib.sha256(BODY).hexdigest()
    assert not destination.with_name("day.gz.part").exists()


@pytest.mark.parametrize("declared", [None, str(len(BODY))])
def test_explicit_size(tmp_path, monkeypatch, declared):
    mock_response(monkeypatch, Response(length=declared))
    assert download_itch.download(DAY, tmp_path / "day.gz", expected_size=len(BODY))[
        "bytes"
    ] == len(BODY)


@pytest.mark.parametrize(
    ("length", "expected"),
    [
        (str(len(BODY) + 1), None),
        (str(len(BODY) - 1), None),
        (None, None),
        ("0", None),
        ("-1", None),
        ("invalid", None),
        (str(len(BODY)), 2),
    ],
)
def test_reject_wrong_or_unverifiable_size(tmp_path, monkeypatch, length, expected):
    mock_response(monkeypatch, Response(length=length))
    destination = tmp_path / "day.gz"
    with pytest.raises(ValueError):
        download_itch.download(DAY, destination, expected_size=expected)
    assert list(tmp_path.iterdir()) == []


@pytest.mark.parametrize("expected", [0, -1, True, 1.5])
def test_invalid_expected_size_never_connects(tmp_path, monkeypatch, expected):
    monkeypatch.setattr(
        download_itch, "urlopen", lambda *a, **kw: pytest.fail("unexpected network")
    )
    with pytest.raises(ValueError, match="positive"):
        download_itch.download(DAY, tmp_path / "day.gz", expected_size=expected)


@pytest.mark.parametrize("existing", ["day.gz", "day.gz.part"])
def test_refuses_existing_files_before_network(tmp_path, monkeypatch, existing):
    monkeypatch.setattr(
        download_itch, "urlopen", lambda *a, **kw: pytest.fail("unexpected network")
    )
    (tmp_path / existing).write_bytes(b"keep")
    with pytest.raises(FileExistsError):
        download_itch.download(DAY, tmp_path / "day.gz")
    assert (tmp_path / existing).read_bytes() == b"keep"


def test_interrupted_download_cleans_partial(tmp_path, monkeypatch):
    class Interrupted(Response):
        def read(self, size):
            if self.tell():
                raise ConnectionError("connection lost")
            return super().read(3)

    mock_response(monkeypatch, Interrupted())
    with pytest.raises(ConnectionError):
        download_itch.download(DAY, tmp_path / "day.gz")
    assert list(tmp_path.iterdir()) == []


def test_http_failure_has_no_output(tmp_path, monkeypatch):
    def fail(*args, **kwargs):
        raise HTTPError("https://example.invalid", 404, "missing", {}, None)

    monkeypatch.setattr(download_itch, "urlopen", fail)
    with pytest.raises(HTTPError):
        download_itch.download(DAY, tmp_path / "day.gz")
    assert list(tmp_path.iterdir()) == []


def test_output_created_during_download_is_preserved(tmp_path, monkeypatch):
    destination = tmp_path / "day.gz"

    class ConcurrentOutput(Response):
        def read(self, size):
            destination.write_bytes(b"keep")
            return super().read(size)

    mock_response(monkeypatch, ConcurrentOutput())
    with pytest.raises(FileExistsError):
        download_itch.download(DAY, destination)
    assert destination.read_bytes() == b"keep"
    assert destination.with_name("day.gz.part").read_bytes() == BODY


def test_hardlink_failure_falls_back_without_losing_data(tmp_path, monkeypatch):
    mock_response(monkeypatch, Response())

    def fail_link(*args):
        raise OSError("hard links unavailable")

    monkeypatch.setattr(Path, "hardlink_to", fail_link)
    destination = tmp_path / "day.gz"
    result = download_itch.download(DAY, destination)
    assert destination.read_bytes() == BODY
    assert result["bytes"] == len(BODY)
    assert not destination.with_name("day.gz.part").exists()


def test_failed_publication_retains_completed_download(tmp_path, monkeypatch):
    mock_response(monkeypatch, Response())

    def fail(*args):
        raise OSError("publication unavailable")

    monkeypatch.setattr(Path, "hardlink_to", fail)
    monkeypatch.setattr("os.replace", fail)
    destination = tmp_path / "day.gz"
    with pytest.raises(OSError):
        download_itch.download(DAY, destination)
    assert not destination.exists()
    assert destination.with_name("day.gz.part").read_bytes() == BODY
