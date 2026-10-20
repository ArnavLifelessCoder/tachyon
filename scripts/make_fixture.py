"""Cut a message prefix or a stock-locate selection from raw or gzip ITCH input."""

import argparse
import gzip
import json
from collections import Counter
from pathlib import Path

MAX_FIXTURE_BYTES = 5_000_000


def filter_fixture(source: Path, destination: Path, locates: set[int]) -> dict:
    """Keep S/R and every selected-locate message in source order from day start.

    Stop before the first retained frame that would reach 5 MB; never skip a
    retained frame to fit a later one. EOF and a zero-length terminator also stop
    the cut. No terminator is appended because this is a filtered fragment.
    """
    if not locates or any(type(x) is not int or not 0 <= x <= 65535 for x in locates):
        raise ValueError("locates must be a nonempty set of unsigned 16-bit integers")
    opener = gzip.open if source.suffix.lower() == ".gz" else Path.open
    total, scanned = 0, 0
    counts, symbols = Counter(), {}
    stop_reason = "eof"
    with opener(source, "rb") as input_file, destination.open("xb") as output:
        try:
            while True:
                prefix = input_file.read(2)
                if not prefix:
                    break
                if len(prefix) != 2:
                    raise EOFError("truncated length prefix")
                length = int.from_bytes(prefix, "big")
                if length == 0:
                    stop_reason = "session_end"
                    break
                payload = input_file.read(length)
                if len(payload) != length or length < 11:
                    raise EOFError("truncated payload or common header")
                locate = int.from_bytes(payload[1:3], "big")
                kind = chr(payload[0])
                if kind in ("S", "R") or locate in locates:
                    if total + 2 + length >= MAX_FIXTURE_BYTES:
                        stop_reason = "size_limit"
                        break
                    if kind == "R":
                        if length != 39:
                            raise ValueError("invalid stock directory length")
                        if locate in locates:
                            symbols[str(locate)] = payload[11:19].decode("ascii").rstrip()
                    output.write(prefix)
                    output.write(payload)
                    total += 2 + length
                    counts[kind] += 1
                scanned += 1
        except BaseException:
            output.close()
            destination.unlink()
            raise
    return {
        "messages": sum(counts.values()),
        "bytes": total,
        "counts": dict(sorted(counts.items())),
        "locates": sorted(locates),
        "symbols": symbols,
        "source_messages_consumed": scanned,
        "stop_reason": stop_reason,
    }


def make_fixture(source: Path, destination: Path, count: int) -> int:
    """Return bytes written; reject truncation and outputs of 5 MB or larger.

    A cut is an incomplete session, so no end-of-session marker is appended.
    The destination must not exist. Failed writes remove only the newly created file.
    """
    if type(count) is not int or count < 0:
        raise ValueError("count must be a nonnegative integer")
    opener = gzip.open if source.suffix.lower() == ".gz" else Path.open
    total = 0
    with opener(source, "rb") as input_file, destination.open("xb") as output:
        try:
            for index in range(count):
                prefix = input_file.read(2)
                if len(prefix) != 2:
                    raise EOFError(f"missing or truncated length at message {index}")
                length = int.from_bytes(prefix, "big")
                if length == 0:
                    raise EOFError(f"session ends after {index} messages; requested {count}")
                if total + 2 + length >= MAX_FIXTURE_BYTES:
                    raise ValueError("fixture must be smaller than 5 MB")
                payload = input_file.read(length)
                if len(payload) != length:
                    raise EOFError(f"truncated payload at message {index}")
                output.write(prefix)
                output.write(payload)
                total += 2 + length
        except BaseException:
            output.close()
            destination.unlink()
            raise
    return total


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--messages", type=int)
    mode.add_argument("--locates", type=int, nargs="+", help="keep S/R and these stock locates")
    args = parser.parse_args()
    if args.locates is not None:
        print(json.dumps(filter_fixture(args.source, args.output, set(args.locates)), indent=2))
    else:
        size = make_fixture(args.source, args.output, args.messages)
        print(f"Wrote {args.messages} messages, {size} bytes to {args.output}")


if __name__ == "__main__":
    main()
