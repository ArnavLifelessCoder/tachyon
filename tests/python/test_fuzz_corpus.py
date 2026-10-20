import hashlib
import json
import random
from pathlib import Path

import pytest

from tests.fuzz_corpus import MUTATIONS, frames, mutate, write_corpus

FIXTURES = Path(__file__).resolve().parents[1] / "fixtures"


def test_frame_boundaries_strip_only_final_terminator():
    data = b"\0\x01A\0\x02BC"
    assert frames(data + b"\0\0") == [b"\0\x01A", b"\0\x02BC"]
    assert b"".join(frames(data)) == data


@pytest.mark.parametrize("data", [b"\0", b"\0\x02A", b"\0\0x"])
def test_reject_bad_seed_framing(data):
    with pytest.raises(ValueError):
        frames(data)


@pytest.mark.parametrize("trial", range(30))
def test_mutation_variants_change_input_deterministically(trial):
    data = b"".join(frames((FIXTURES / "synthetic.itch").read_bytes()))
    actual = mutate(data, random.Random(42), trial)
    assert actual == mutate(data, random.Random(42), trial)
    assert actual[0] != data
    assert actual[1] == MUTATIONS[trial % len(MUTATIONS)]


def test_written_corpus_has_reproducible_seeds_and_hashes(tmp_path):
    first, second = tmp_path / "first", tmp_path / "second"
    entries = write_corpus(FIXTURES, first, seed=123, count=30)
    assert entries == write_corpus(FIXTURES, second, seed=123, count=30)
    assert set(x["mutation"] for x in entries) == set(MUTATIONS)
    assert json.loads((first / "manifest.json").read_text()) == entries
    for entry in entries:
        data = (first / entry["file"]).read_bytes()
        assert data == (second / entry["file"]).read_bytes()
        assert len(data) == entry["bytes"]
        assert hashlib.sha256(data).hexdigest() == entry["sha256"]
