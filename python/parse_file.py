"""Run the mapped C++ parser, decompressing gzip input before parsing."""

import argparse
import gzip
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def run_parser(source: Path, executable: Path, *, events=False):
    """Return captured parser output; always remove this invocation's scratch."""
    command = [str(executable.resolve())]
    options = ["--events"] if events else []
    if source.suffix.lower() != ".gz":
        return subprocess.run(
            [*command, str(source.resolve()), *options], capture_output=True, text=True, timeout=120
        )
    scratch = ROOT / ".tmp"
    scratch.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="itch-", dir=scratch) as directory:
        raw = Path(directory) / "input.itch"
        with gzip.open(source, "rb") as compressed, raw.open("xb") as output:
            shutil.copyfileobj(compressed, output, length=1024 * 1024)
        return subprocess.run(
            [*command, str(raw), *options], capture_output=True, text=True, timeout=120
        )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("--parser", type=Path, required=True, help="path to itch_parse executable")
    parser.add_argument("--events", action="store_true")
    args = parser.parse_args()
    result = run_parser(args.input, args.parser, events=args.events)
    print(result.stdout, end="")
    if result.stderr:
        import sys

        print(result.stderr, end="", file=sys.stderr)
    raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
