"""Independent, deliberately simple ITCH 5.0 parser using struct.unpack."""

import argparse
import json
import struct
from collections import Counter
from pathlib import Path

# Body formats follow Nasdaq's March 6, 2015 specification; no writer imports.
FORMATS = {
    "S": ("c", "event_code"),
    "R": (
        "8sccIcc2scccccIc",
        "stock market_category financial_status round_lot_size round_lots_only "
        "issue_classification issue_subtype authenticity short_sale_threshold ipo_flag "
        "luld_tier etp_flag etp_leverage_factor inverse_indicator",
    ),
    "A": ("QcI8sI", "order_id side shares stock price"),
    "F": ("QcI8sI4s", "order_id side shares stock price attribution"),
    "E": ("QIQ", "order_id shares match_number"),
    "C": ("QIQcI", "order_id shares match_number printable price"),
    "X": ("QI", "order_id shares"),
    "D": ("Q", "order_id"),
    "U": ("QQII", "order_id new_order_id shares price"),
    "P": ("QcI8sIQ", "order_id side shares stock price match_number"),
}


class ParseError(ValueError):
    def __init__(self, code, offset):
        self.code, self.offset = code, offset
        super().__init__(f"{code} at byte {offset}")


def parse(data: bytes, *, events: bool = False) -> dict:
    """Accept a complete fragment or a final zero-length session terminator.

    Validate framing and exact known-type lengths, not trading semantics or
    alphabet restrictions. Unknown payloads are opaque, even without a header.
    """
    offset = 0
    counts, skipped = Counter(), Counter()
    decoded = []
    terminated = False
    while offset < len(data):
        start = offset
        if len(data) - offset < 2:
            raise ParseError("truncated_prefix", start)
        length = struct.unpack_from(">H", data, offset)[0]
        offset += 2
        if length == 0:
            if offset != len(data):
                raise ParseError("trailing_data", start)
            terminated = True
            break
        if length > len(data) - offset:
            raise ParseError("truncated_payload", start)
        payload = data[offset : offset + length]
        kind = chr(payload[0])
        if kind in FORMATS:
            fmt, names = FORMATS[kind]
            if length != 11 + struct.calcsize(">" + fmt):
                raise ParseError("invalid_length", start)
            locate, tracking, timestamp = struct.unpack(">HH6s", payload[1:11])
            event = {
                "type": kind,
                "stock_locate": locate,
                "tracking_number": tracking,
                "timestamp": int.from_bytes(timestamp, "big"),
            }
            for name, value in zip(
                names.split(), struct.unpack(">" + fmt, payload[11:]), strict=True
            ):
                event[name] = value.decode("latin1") if isinstance(value, bytes) else value
        else:
            skipped[kind] += 1
            event = {"type": kind, "payload": payload.hex()}
        if events:
            decoded.append(event)
        counts[kind] += 1
        offset += length
    result = {
        "messages": sum(counts.values()),
        "counts": dict(sorted(counts.items())),
        "skipped": dict(sorted(skipped.items())),
        "bytes": offset,
        "terminated": terminated,
    }
    if events:
        result["events"] = decoded
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("--events", action="store_true")
    args = parser.parse_args()
    try:
        print(json.dumps(parse(args.input.read_bytes(), events=args.events)))
    except ParseError as error:
        print(json.dumps({"error": error.code, "offset": error.offset}))
        raise SystemExit(2) from error


if __name__ == "__main__":
    main()
