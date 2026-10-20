"""Download one Nasdaq ITCH day and verify the compressed file's byte count."""

import argparse
import hashlib
import json
from datetime import date
from pathlib import Path
from urllib.request import urlopen

BASE_URL = "https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/"
CHUNK_SIZE = 1024 * 1024


def download(day: date, destination: Path, *, expected_size: int | None = None) -> dict:
    """Stream to a sibling .part file and publish only after size validation.

    Content-Length is checked whenever present. Without it, --expected-size
    is required. Existing outputs and partial downloads are never overwritten.
    """
    if expected_size is not None and (type(expected_size) is not int or expected_size <= 0):
        raise ValueError("expected_size must be a positive integer")
    url = f"{BASE_URL}{day:%m%d%Y}.NASDAQ_ITCH50.gz"
    partial = destination.with_name(destination.name + ".part")
    if destination.exists() or partial.exists():
        raise FileExistsError(f"output or partial file already exists: {destination}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with urlopen(url, timeout=60) as response:
        length_header = response.headers.get("Content-Length")
        declared_size = int(length_header) if length_header is not None else None
        if declared_size is not None and declared_size <= 0:
            raise ValueError("Content-Length must be positive")
        if declared_size is None and expected_size is None:
            raise ValueError("server omitted Content-Length; supply --expected-size")
        if expected_size is not None and declared_size not in (None, expected_size):
            raise ValueError("Content-Length differs from expected_size")
        target_size = expected_size if expected_size is not None else declared_size
        digest = hashlib.sha256()
        total = 0
        with partial.open("xb") as output:
            try:
                while chunk := response.read(CHUNK_SIZE):
                    total += len(chunk)
                    if total > target_size:
                        raise ValueError(f"download exceeds expected size {target_size}")
                    output.write(chunk)
                    digest.update(chunk)
                if total != target_size:
                    raise ValueError(f"size mismatch: expected {target_size}, received {total}")
            except BaseException:
                output.close()
                partial.unlink()
                raise
    # Exclusive creation also protects against an output created during the download.
    # A hard link publishes the already-complete file on both Windows and POSIX.
    try:
        destination.hardlink_to(partial)
    finally:
        partial.unlink()
    return {"url": url, "bytes": total, "sha256": digest.hexdigest(), "path": str(destination)}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("day", type=date.fromisoformat, help="YYYY-MM-DD")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--expected-size", type=int, help="compressed bytes")
    args = parser.parse_args()
    destination = args.output or Path("data") / f"{args.day:%m%d%Y}.NASDAQ_ITCH50.gz"
    print(json.dumps(download(args.day, destination, expected_size=args.expected_size), indent=2))


if __name__ == "__main__":
    main()
