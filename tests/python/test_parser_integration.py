import gzip
import hashlib
import json
import random

import pytest

from python.itch_writer import HEADER, SCHEMAS, encode_event, frame
from python.parse_file import ROOT, run_parser, run_parser_batch
from python.ref_parser import ParseError, parse
from tests.fuzz_corpus import write_corpus

FIXTURES = ROOT / "tests" / "fixtures"
FIXTURE_NAMES = ["synthetic", "01302019.first100000", "01302019.AAPL-MSFT-SPY"]
MALFORMED_CASES = [
    b"\0",
    b"\xff\xff?",
    frame(b"S"),
    frame(b"R" + bytes(39)),
    b"\0\0x",
    frame(b"?") + b"\0\x10S",
]


def batch_rows(sources, executable, *, events=True):
    result = run_parser_batch(sources, executable, events=events)
    assert result.returncode == 0, result.stdout + result.stderr
    rows = [json.loads(line) for line in result.stdout.splitlines()]
    assert len(rows) == len(sources), f"inputs={sources}"
    return rows


@pytest.fixture(scope="module")
def fixture_counts(parser_executable):
    rows = batch_rows(
        [FIXTURES / f"{name}.itch" for name in FIXTURE_NAMES], parser_executable, events=False
    )
    assert all(row["status"] == 0 for row in rows), rows
    return {name: row["result"] for name, row in zip(FIXTURE_NAMES, rows, strict=True)}


def cpp(source, executable, *, events=False, context=""):
    try:
        result = run_parser(source, executable, events=events)
        assert result.returncode == 0, result.stdout + result.stderr
        return json.loads(result.stdout)
    except Exception as error:
        error.add_note(f"{context} input={source}")
        raise


@pytest.mark.parametrize("name", FIXTURE_NAMES)
def test_cpp_reference_and_manifest_counts(fixture_counts, name):
    source = FIXTURES / f"{name}.itch"
    manifest = json.loads((FIXTURES / f"{name}.manifest.json").read_text())
    actual = fixture_counts[name]
    assert actual == parse(source.read_bytes())
    assert actual["counts"] == manifest["counts"]
    assert actual["messages"] == manifest["messages"]
    if name == "01302019.AAPL-MSFT-SPY":
        assert all(actual["counts"][kind] for kind in "AFECXDUP")


def test_seeded_field_differential(parser_executable, tmp_path, request):
    base = request.config.getoption("--seed")
    sources = []
    for seed in range(base, base + 200):
        source = tmp_path / f"seed-{seed}.itch"
        rng = random.Random(seed)
        frames = []
        for _ in range(500):
            kind = rng.choice(list(SCHEMAS))
            event = {"type": kind}
            for field in (*HEADER, *SCHEMAS[kind]):
                if field.kind == "uint":
                    value = rng.getrandbits(field.width * 8)
                else:
                    value = "".join(chr(rng.randrange(32, 127)) for _ in range(field.width))
                event[field.name] = value
            frames.append(encode_event(event))
            if rng.randrange(10) == 0:
                frames.append(frame(b"\xff" + rng.randbytes(rng.randrange(40))))
        data = b"".join(frames) + (b"\0\0" if seed % 2 else b"")
        source.write_bytes(data)
        sources.append(source)
    result = run_parser_batch(sources, parser_executable, events=True)
    assert result.returncode == 0, result.stdout + result.stderr
    rows = [json.loads(line) for line in result.stdout.splitlines()]
    assert len(rows) == len(sources), f"seed={base} inputs={tmp_path}"
    for source, row in zip(sources, rows, strict=True):
        assert row == {"status": 0, "result": parse(source.read_bytes(), events=True)}, str(source)


def test_mutated_corpus_differential(parser_executable, tmp_path, request):
    directory = tmp_path / "mutated-corpus"
    base = request.config.getoption("--seed")
    entries = write_corpus(FIXTURES, directory, seed=base, count=200)
    result = run_parser_batch(
        [directory / entry["file"] for entry in entries], parser_executable, events=True
    )
    assert result.returncode == 0, result.stdout + result.stderr
    rows = [json.loads(line) for line in result.stdout.splitlines()]
    assert len(rows) == len(entries), f"seed={base} inputs={directory}"
    successes, errors = 0, 0
    for entry, row in zip(entries, rows, strict=True):
        source = directory / entry["file"]
        context = f"seed={entry['seed']} mutation={entry['mutation']} input={source}"
        try:
            try:
                expected = parse(source.read_bytes(), events=True)
                expected_status = 0
                successes += 1
            except ParseError as error:
                expected = {"error": error.code, "offset": error.offset}
                expected_status = 2
                errors += 1
            assert row == {"status": expected_status, "result": expected}
        except Exception as error:
            error.add_note(context)
            raise
    assert successes > 0 and errors > 0, f"seed={base} successes={successes} errors={errors}"
    (directory / "comparison.json").write_text(
        json.dumps(
            {"seed": base, "streams": len(entries), "successes": successes, "errors": errors}
        )
        + "\n",
        encoding="utf-8",
    )


@pytest.fixture(scope="module")
def boundary_results(parser_executable, tmp_path_factory):
    # Arbitrary payload bytes are structural inputs, not valid trading events.
    import struct

    from python.ref_parser import FORMATS

    directory = tmp_path_factory.mktemp("binary-boundaries")
    sources = []
    inputs = []
    for kind in SCHEMAS:
        length = 11 + struct.calcsize(">" + FORMATS[kind][0])
        data = frame(kind.encode() + b"\xff" * (length - 1))
        source = directory / f"{kind}.itch"
        source.write_bytes(data)
        sources.append(source)
        inputs.append(data)
    rows = batch_rows(sources, parser_executable)
    return dict(zip(SCHEMAS, zip(inputs, rows, strict=True), strict=True))


@pytest.mark.parametrize("kind", list(SCHEMAS))
def test_binary_text_and_numeric_boundaries(boundary_results, kind):
    data, row = boundary_results[kind]
    assert row == {"status": 0, "result": parse(data, events=True)}


@pytest.fixture(scope="module")
def error_results(parser_executable, tmp_path_factory):
    directory = tmp_path_factory.mktemp("malformed-frames")
    sources = []
    for index, data in enumerate(MALFORMED_CASES):
        source = directory / f"invalid-{index}.itch"
        source.write_bytes(data)
        sources.append(source)
    return dict(zip(MALFORMED_CASES, batch_rows(sources, parser_executable), strict=True))


@pytest.mark.parametrize("data", MALFORMED_CASES)
def test_error_code_and_offset_match_reference(error_results, data):
    with pytest.raises(ParseError) as info:
        parse(data)
    assert error_results[data] == {
        "status": 2,
        "result": {"error": info.value.code, "offset": info.value.offset},
    }


@pytest.mark.parametrize("compressed", [False, True])
def test_unicode_path_and_empty_input(parser_executable, tmp_path, compressed):
    source = tmp_path / ("行情 λ.gz" if compressed else "行情 λ.itch")
    source.write_bytes(gzip.compress(b"") if compressed else b"")
    assert cpp(source, parser_executable) == parse(b"")


def test_gzip_members_and_cleanup(parser_executable, tmp_path):
    first = encode_event({"type": "S", "event_code": "O"})
    second = encode_event({"type": "S", "event_code": "C"})
    source = tmp_path / "input.gz"
    source.write_bytes(gzip.compress(first) + gzip.compress(second))
    before = set((ROOT / ".tmp").glob("itch-*"))
    original = hashlib.sha256(source.read_bytes()).hexdigest()
    assert cpp(source, parser_executable, events=True) == parse(first + second, events=True)
    assert set((ROOT / ".tmp").glob("itch-*")) == before
    assert hashlib.sha256(source.read_bytes()).hexdigest() == original


@pytest.mark.parametrize("mode", ["crc", "truncated", "parse_error"])
def test_gzip_failure_cleans_temporary_file(parser_executable, tmp_path, mode):
    data = bytearray(gzip.compress(b"\0" if mode == "parse_error" else b"\0\0"))
    if mode == "crc":
        data[-8] ^= 1
    elif mode == "truncated":
        del data[-4:]
    source = tmp_path / "bad.gz"
    source.write_bytes(data)
    before = set((ROOT / ".tmp").glob("itch-*"))
    if mode == "parse_error":
        assert run_parser(source, parser_executable, expected_status=2).returncode == 2
    else:
        with pytest.raises((gzip.BadGzipFile, EOFError)):
            run_parser(source, parser_executable)
    assert set((ROOT / ".tmp").glob("itch-*")) == before


def test_cli_missing_input(parser_executable, tmp_path):
    result = run_parser(tmp_path / "missing", parser_executable, expected_status=1)
    assert result.returncode == 1
    assert result.stderr
