"""Run a full ITCH day through the single-threaded and threaded pipelines.

Records whether the final books and the published BBO sequence are identical,
plus throughput and parse-to-book latency for both runs, with machine info.
"""

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from scripts.machine_info import collect  # noqa: E402
from scripts.replay_day import MANIFEST, raw_day  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pipeline", type=Path, required=True, help="release pipeline_replay")
    parser.add_argument("--source", type=Path, default=ROOT / "data/01302019.NASDAQ_ITCH50.gz")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cpus", nargs=3, type=int, default=[2, 4, 6], metavar="CPU")
    parser.add_argument("--flags", default="release preset")
    args = parser.parse_args()
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    raw = raw_day(args.source, manifest["source_uncompressed_bytes"])
    command = [str(args.pipeline.resolve()), str(raw), *map(str, args.cpus)]
    result = subprocess.run(command, capture_output=True, text=True, timeout=7200)
    if result.returncode not in (0, 4):
        sys.exit(f"pipeline_replay failed ({result.returncode}): {result.stderr}")
    record = {
        "source_file": args.source.name,
        "source_sha256": manifest["source_sha256"],
        "command": "python scripts/pipeline_day.py --pipeline "
        f"{args.pipeline.as_posix()} --cpus {' '.join(map(str, args.cpus))}",
        "result": json.loads(result.stdout),
        "machine": collect(args.flags),
        "date": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    summary = record["result"]
    print(json.dumps({k: summary[k] for k in ("books_identical", "bbo_sequence_identical")}))
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
