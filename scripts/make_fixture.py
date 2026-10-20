"""Copy exactly the first N framed messages from raw or gzip ITCH input."""

import argparse
import gzip
from pathlib import Path

MAX_FIXTURE_BYTES = 5_000_000


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
    parser.add_argument("--messages", type=int, required=True)
    args = parser.parse_args()
    size = make_fixture(args.source, args.output, args.messages)
    print(f"Wrote {args.messages} messages, {size} bytes to {args.output}")


if __name__ == "__main__":
    main()
