"""Characterize parser process hangs; stop at the first failure without retrying."""

import argparse
import hashlib
import json
import platform
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from python.parse_file import run_parser  # noqa: E402


def probe(source, executable, launches):
    records = []
    for index in range(launches):
        started = time.perf_counter()
        try:
            result = run_parser(source, executable)
            status = "passed" if result.returncode == 0 else "failed"
            stderr = result.stderr
            returncode = result.returncode
        except subprocess.TimeoutExpired as error:
            status = "timeout"
            stderr = (error.stderr or b"").decode("utf-8", errors="replace")
            returncode = None
        record = {
            "launch": index + 1,
            "status": status,
            "seconds": time.perf_counter() - started,
            "entered_main": "itch_parse: main entered" in stderr,
            "returning_main": "itch_parse: main returning status=" in stderr,
            "returncode": returncode,
        }
        records.append(record)
        if status != "passed":
            record["stderr"] = stderr
            break
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--parser", type=Path, required=True)
    parser.add_argument("--input", type=Path, default=ROOT / "tests/fixtures/synthetic.itch")
    parser.add_argument("--launches", type=int, default=1000)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.launches < 1:
        parser.error("--launches must be positive")
    records = probe(args.input, args.parser, args.launches)
    report = {
        "platform": platform.platform(),
        "python": platform.python_version(),
        "executable": args.parser.relative_to(ROOT).as_posix()
        if args.parser.is_absolute()
        else args.parser.as_posix(),
        "fixture": args.input.name,
        "fixture_sha256": hashlib.sha256(args.input.read_bytes()).hexdigest(),
        "requested_launches": args.launches,
        "timeout_seconds": 20,
        "stop_on_first_failure": True,
        "records": records,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(records[-1], indent=2))
    return 0 if records[-1]["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
