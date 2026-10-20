"""Encode synthetic ITCH 5.0 events in Nasdaq BinaryFILE framing.

Prices are unsigned integers in units of 0.0001. This is a wire encoder:
it validates field widths and types, not order lifecycle or exchange rules.
"""

import argparse
import json
from collections.abc import Iterable, Mapping
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Field:
    name: str
    width: int
    kind: str = "uint"
    default: int | str | None = None


HEADER = (
    Field("stock_locate", 2, default=0),
    Field("tracking_number", 2, default=0),
    Field("timestamp", 6, default=0),
)
ORDER = (Field("order_id", 8),)
ADD = (
    *ORDER,
    Field("side", 1, "text"),
    Field("shares", 4),
    Field("stock", 8, "text"),
    Field("price", 4),
)
EXECUTE = (*ORDER, Field("shares", 4), Field("match_number", 8))
SCHEMAS = {
    "S": (Field("event_code", 1, "text"),),
    "R": (
        Field("stock", 8, "text"),
        Field("market_category", 1, "text", "Q"),
        Field("financial_status", 1, "text", "N"),
        Field("round_lot_size", 4, default=100),
        Field("round_lots_only", 1, "text", "N"),
        Field("issue_classification", 1, "text", "C"),
        Field("issue_subtype", 2, "text", " "),
        Field("authenticity", 1, "text", "T"),
        Field("short_sale_threshold", 1, "text", "N"),
        Field("ipo_flag", 1, "text", "N"),
        Field("luld_tier", 1, "text", "1"),
        Field("etp_flag", 1, "text", "N"),
        Field("etp_leverage_factor", 4, default=0),
        Field("inverse_indicator", 1, "text", "N"),
    ),
    "A": ADD,
    "F": (*ADD, Field("attribution", 4, "text")),
    "E": EXECUTE,
    "C": (*EXECUTE, Field("printable", 1, "text"), Field("price", 4)),
    "X": (*ORDER, Field("shares", 4)),
    "D": ORDER,
    "U": (*ORDER, Field("new_order_id", 8), Field("shares", 4), Field("price", 4)),
    "P": (*ADD, Field("match_number", 8)),
}


def frame(payload: bytes) -> bytes:
    """Frame a nonempty raw payload, including message types outside SCHEMAS."""
    if not isinstance(payload, bytes) or not 1 <= len(payload) <= 65535:
        raise ValueError("payload must be bytes with length in [1, 65535]")
    return len(payload).to_bytes(2, "big") + payload


def _encode_field(field: Field, value: object) -> bytes:
    if field.kind == "uint":
        if type(value) is not int or not 0 <= value < 1 << (8 * field.width):
            raise ValueError(f"{field.name} must be an unsigned {8 * field.width}-bit integer")
        return value.to_bytes(field.width, "big")
    if not isinstance(value, str) or not 1 <= len(value) <= field.width:
        raise ValueError(f"{field.name} must have 1 to {field.width} ASCII characters")
    if any(not 32 <= ord(char) <= 126 for char in value):
        raise ValueError(f"{field.name} must contain printable ASCII only")
    return value.encode("ascii").ljust(field.width, b" ")


def encode_event(event: Mapping[str, object]) -> bytes:
    """Encode one event, rejecting missing or misspelled fields."""
    message_type = event.get("type")
    if not isinstance(message_type, str) or message_type not in SCHEMAS:
        raise ValueError(f"unsupported message type: {message_type!r}")
    fields = (*HEADER, *SCHEMAS[message_type])
    extras = event.keys() - {"type", *(field.name for field in fields)}
    if extras:
        raise ValueError(f"unknown fields: {sorted(extras)}")
    payload = message_type.encode("ascii")
    for field in fields:
        value = event.get(field.name, field.default)
        if value is None:
            raise ValueError(f"missing field: {field.name}")
        payload += _encode_field(field, value)
    return frame(payload)


def encode_events(events: Iterable[Mapping[str, object]], *, end_session: bool = False) -> bytes:
    """Encode events; an optional zero-length frame marks a complete session."""
    return b"".join(map(encode_event, events)) + (b"\x00\x00" if end_session else b"")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("events", type=Path, help="JSON list of event objects")
    parser.add_argument("output", type=Path)
    parser.add_argument("--end-session", action="store_true")
    args = parser.parse_args()
    events = json.loads(args.events.read_text(encoding="utf-8"))
    data = encode_events(events, end_session=args.end_session)
    with args.output.open("xb") as output:
        output.write(data)
    print(f"Wrote {len(events)} messages, {len(data)} bytes to {args.output}")


if __name__ == "__main__":
    main()
