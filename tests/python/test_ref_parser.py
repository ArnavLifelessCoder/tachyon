import json
from pathlib import Path

import pytest

from python.itch_writer import HEADER, SCHEMAS, encode_event, encode_events, frame
from python.ref_parser import ParseError, parse

FIXTURES = Path(__file__).resolve().parents[1] / "fixtures"


@pytest.mark.parametrize("kind", list(SCHEMAS))
def test_decode_every_field(kind):
    event = {"type": kind}
    expected = {"type": kind}
    for field in (*HEADER, *SCHEMAS[kind]):
        value = (1 << (8 * field.width)) - 1 if field.kind == "uint" else "Z"
        event[field.name] = value
        expected[field.name] = value if field.kind == "uint" else value.ljust(field.width)
    result = parse(encode_event(event), events=True)
    assert result["events"] == [expected]
    assert result["counts"] == {kind: 1}
    assert result["skipped"] == {}


@pytest.mark.parametrize("name", ["synthetic", "01302019.first100000", "01302019.AAPL-MSFT-SPY"])
def test_manifest_counts(name):
    manifest = json.loads((FIXTURES / f"{name}.manifest.json").read_text())
    result = parse((FIXTURES / f"{name}.itch").read_bytes())
    assert result["counts"] == manifest["counts"]
    assert result["messages"] == manifest["messages"]


def test_unknown_types_and_terminator():
    data = frame(b"?") + frame(b"\xffabcdef") + b"\0\0"
    result = parse(data, events=True)
    assert result["skipped"] == {"?": 1, "\xff": 1}
    assert result["events"] == [
        {"type": "?", "payload": "3f"},
        {"type": "\xff", "payload": "ff616263646566"},
    ]
    assert result["terminated"]
    assert result["bytes"] == len(data)


@pytest.mark.parametrize(
    ("data", "code"),
    [
        (b"\0", "truncated_prefix"),
        (b"\0\x03?", "truncated_payload"),
        (frame(b"S"), "invalid_length"),
        (b"\0\0x", "trailing_data"),
    ],
)
def test_errors_report_frame_start(data, code):
    prefix = encode_events([{"type": "S", "event_code": "O"}])
    with pytest.raises(ParseError) as info:
        parse(prefix + data)
    assert (info.value.code, info.value.offset) == (code, len(prefix))


def test_empty_and_frame_boundary_fragments():
    assert parse(b"")["messages"] == 0
    assert not parse(b"")["terminated"]
    assert parse(b"\0\0")["terminated"]
