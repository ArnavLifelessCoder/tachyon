"""Replay a full ITCH day through book_replay and record the result.

The compressed day is decompressed once into data/ (ignored by git); the raw
file is reused on later runs after its size is checked. The replay output plus
machine info is written as JSON for results/.
"""

import argparse
import gzip
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from scripts.machine_info import collect  # noqa: E402

MANIFEST = ROOT / "tests" / "fixtures" / "01302019.AAPL-MSFT-SPY.manifest.json"


def raw_day(source: Path, expected_bytes: int) -> Path:
    raw = source.with_suffix("")  # 01302019.NASDAQ_ITCH50.gz -> 01302019.NASDAQ_ITCH50
    if raw.exists() and raw.stat().st_size == expected_bytes:
        return raw
    partial = raw.with_name(raw.name + ".part")
    with gzip.open(source, "rb") as compressed, partial.open("wb") as output:
        shutil.copyfileobj(compressed, output, length=16 * 1024 * 1024)
    if partial.stat().st_size != expected_bytes:
        raise ValueError(f"decompressed {partial.stat().st_size} bytes, expected {expected_bytes}")
    partial.replace(raw)
    return raw


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--replay", type=Path, required=True, help="release book_replay executable")
    parser.add_argument("--source", type=Path, default=ROOT / "data/01302019.NASDAQ_ITCH50.gz")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--flags", default="release preset")
    args = parser.parse_args()
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    started = time.perf_counter()
    raw = raw_day(args.source, manifest["source_uncompressed_bytes"])
    prepared = time.perf_counter() - started
    result = subprocess.run(
        [str(args.replay.resolve()), str(raw)], capture_output=True, text=True, timeout=3600
    )
    if result.returncode not in (0, 3):
        sys.exit(f"book_replay failed ({result.returncode}): {result.stderr}")
    record = {
        "source_file": args.source.name,
        "source_sha256": manifest["source_sha256"],
        "source_uncompressed_bytes": manifest["source_uncompressed_bytes"],
        "command": f"python scripts/replay_day.py --replay {args.replay.as_posix()}",
        "decompress_seconds": round(prepared, 2),
        "replay": json.loads(result.stdout),
        "machine": collect(args.flags),
        "date": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k: record["replay"][k] for k in ("messages", "unknown_refs")}))
    return 0 if record["replay"]["unknown_refs"] == 0 else 3


if __name__ == "__main__":
    raise SystemExit(main())
