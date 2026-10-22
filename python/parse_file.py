"""Run the mapped C++ parser, decompressing gzip input before parsing."""

import argparse
import gzip
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from scripts.stall_policy import SANITIZER, process_timeout  # noqa: E402


def _invoke(command, sources, *, input_bytes=None, expected_status=0):
    scratch = ROOT / ".tmp"
    scratch.mkdir(exist_ok=True)
    # Files let timeout diagnostics be read without waiting for pipe EOF.
    with (
        tempfile.TemporaryFile(dir=scratch) as output,
        tempfile.TemporaryFile(dir=scratch) as diagnostics,
    ):
        try:
            result = subprocess.run(
                command, input=input_bytes, stdout=output, stderr=diagnostics, timeout=20
            )
        except subprocess.TimeoutExpired as error:
            output.seek(0)
            diagnostics.seek(0)
            error.output = output.read()
            error.stderr = diagnostics.read()
            stderr = error.stderr.decode("utf-8", errors="replace")
            error.me_timeout = process_timeout(
                Path(command[0]).stem,
                expected_status,
                stderr,
                error.output.decode("utf-8", errors="replace"),
                20,
            )
            indices = re.findall(r"itch_parse: input index=(\d+)", stderr)
            if indices and int(indices[-1]) < len(sources):
                error.add_note(f"active input={sources[int(indices[-1])]}")
            else:
                error.add_note("inputs=" + ", ".join(str(source) for source in sources))
            error.add_note("stderr:\n" + stderr)
            raise
        output.seek(0)
        diagnostics.seek(0)
        result = subprocess.CompletedProcess(
            command,
            result.returncode,
            output.read().decode("utf-8", errors="replace"),
            diagnostics.read().decode("utf-8", errors="replace"),
        )
        if SANITIZER.search(result.stdout + result.stderr):
            raise RuntimeError("Sanitizer report from child:\n" + result.stdout + result.stderr)
        return result


def run_parser_batch(sources, executable: Path, *, events=False):
    """Parse raw files in one process; return one JSON line per input in order."""
    paths = [Path(source).resolve() for source in sources]
    if any("\n" in str(path) or "\r" in str(path) for path in paths):
        raise ValueError("batch paths must not contain line breaks")
    command = [str(executable.resolve()), "--batch"]
    if events:
        command.append("--events")
    return _invoke(
        command, paths, input_bytes="".join(f"{path}\n" for path in paths).encode("utf-8")
    )


def run_parser(source: Path, executable: Path, *, events=False, expected_status=0):
    """Return captured parser output; always remove this invocation's scratch."""
    command = [str(executable.resolve())]
    options = ["--events"] if events else []
    if source.suffix.lower() != ".gz":
        return _invoke(
            [*command, str(source.resolve()), *options], [source], expected_status=expected_status
        )
    scratch = ROOT / ".tmp"
    scratch.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="itch-", dir=scratch) as directory:
        raw = Path(directory) / "input.itch"
        with gzip.open(source, "rb") as compressed, raw.open("xb") as output:
            shutil.copyfileobj(compressed, output, length=1024 * 1024)
        return _invoke([*command, str(raw), *options], [source], expected_status=expected_status)


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
