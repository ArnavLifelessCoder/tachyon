"""Reproducible malformed streams derived from complete fixture frames."""

import hashlib
import json
import random
from pathlib import Path

MUTATIONS = ("flip", "length", "type", "truncate", "insert", "delete")


def frames(data: bytes) -> list[bytes]:
    result = []
    offset = 0
    while offset < len(data):
        if len(data) - offset < 2:
            raise ValueError("truncated seed prefix")
        length = int.from_bytes(data[offset : offset + 2], "big")
        if length == 0:
            if offset + 2 != len(data):
                raise ValueError("nonfinal seed terminator")
            break
        end = offset + 2 + length
        if end > len(data):
            raise ValueError("truncated seed payload")
        result.append(data[offset:end])
        offset = end
    return result


def mutate(data: bytes, rng: random.Random, trial: int) -> tuple[bytes, str]:
    """Apply one mutation anywhere in the stream, including its first frame."""
    parts = frames(data)
    offsets = []
    offset = 0
    for part in parts:
        offsets.append(offset)
        offset += len(part)
    out = bytearray(data)
    frame = rng.choice(offsets)
    position = rng.randrange(len(out))
    mutation = MUTATIONS[trial % len(MUTATIONS)]
    if mutation == "flip":
        for index in rng.sample(range(len(out)), min(len(out), rng.randint(1, 4))):
            out[index] ^= 1 << rng.randrange(8)
    elif mutation == "length":
        original = int.from_bytes(out[frame : frame + 2], "big")
        variants = ((original - 1) & 65535, (original + 1) & 65535, 0, 65535, rng.randrange(65536))
        length = variants[(trial // len(MUTATIONS)) % len(variants)]
        if length == original:
            length ^= 1
        out[frame : frame + 2] = length.to_bytes(2, "big")
    elif mutation == "type":
        out[frame + 2] ^= rng.randrange(1, 256)
    elif mutation == "truncate":
        del out[position:]
    elif mutation == "insert":
        out[position:position] = rng.randbytes(rng.randint(1, 8))
    else:
        del out[position : position + rng.randint(1, 8)]
    return bytes(out), mutation


def write_corpus(fixtures: Path, destination: Path, *, seed: int, count: int = 200) -> list[dict]:
    """Write all inputs and their seeds/hashes before running either decoder."""
    synthetic = frames((fixtures / "synthetic.itch").read_bytes())
    real = frames((fixtures / "01302019.AAPL-MSFT-SPY.itch").read_bytes())
    destination.mkdir()
    entries = []
    for trial in range(count):
        case_seed = seed + trial
        rng = random.Random(case_seed)
        parts = synthetic.copy() if trial % 4 != 1 else []
        if trial % 4 != 0:
            size = rng.randint(8, 64)
            first = rng.randrange(len(real) - size + 1)
            parts.extend(real[first : first + size])
        data, mutation = mutate(b"".join(parts), rng, trial)
        filename = f"seed-{case_seed}.itch"
        (destination / filename).write_bytes(data)
        entries.append(
            {
                "file": filename,
                "seed": case_seed,
                "mutation": mutation,
                "bytes": len(data),
                "sha256": hashlib.sha256(data).hexdigest(),
            }
        )
    (destination / "manifest.json").write_text(
        json.dumps(entries, indent=2) + "\n", encoding="utf-8"
    )
    return entries
