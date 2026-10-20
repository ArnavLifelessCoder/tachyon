"""Resolve the allocation probe's input and expected count from its manifest."""

import argparse
import json
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from scripts.stall_policy import SANITIZER  # noqa: E402


def run_probe(executable: Path, manifest_path: Path) -> subprocess.CompletedProcess:
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    expected = manifest["messages"]
    if type(expected) is not int or not 0 <= expected <= ((1 << 64) - 1) // 10:
        raise ValueError("manifest messages must be a nonnegative count that fits ten replays")
    source = manifest_path.parent / manifest["fixture_file"]
    result = subprocess.run(
        [str(executable.resolve()), str(source.resolve()), str(expected)],
        capture_output=True,
        text=True,
        timeout=60,
    )
    if SANITIZER.search(result.stdout + result.stderr):
        raise RuntimeError("Sanitizer report from child:\n" + result.stdout + result.stderr)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("manifest", type=Path)
    args = parser.parse_args()
    result = run_probe(args.executable, args.manifest)
    print(result.stdout, end="")
    print(result.stderr, end="", file=sys.stderr)
    raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
