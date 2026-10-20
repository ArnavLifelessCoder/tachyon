import pytest

from python.itch_writer import encode_event, encode_events, frame

# Independently transcribed from the Nasdaq ITCH 5.0 field-offset tables.
HEADER = bytes.fromhex("1234 5678 010203040506")
ORDER_ID = 0x0102030405060708
MATCH = 0x1112131415161718
ADD = dict(order_id=ORDER_ID, side="B", shares=0x11223344, stock="AB", price=0x55667788)
ADD_BYTES = bytes.fromhex("0102030405060708") + b"B" + bytes.fromhex("11223344")
ADD_BYTES += b"AB      " + bytes.fromhex("55667788")


@pytest.mark.parametrize(
    ("kind", "fields", "body", "length"),
    [
        ("S", {"event_code": "O"}, b"O", 12),
        ("R", {"stock": "AB"}, b"AB      QN\x00\x00\x00dNC  TNN1N\x00\x00\x00\x00N", 39),
        ("A", ADD, ADD_BYTES, 36),
        ("F", {**ADD, "attribution": "TEST"}, ADD_BYTES + b"TEST", 40),
        (
            "E",
            dict(order_id=ORDER_ID, shares=0x11223344, match_number=MATCH),
            bytes.fromhex("0102030405060708 11223344 1112131415161718"),
            31,
        ),
        (
            "C",
            dict(
                order_id=ORDER_ID,
                shares=0x11223344,
                match_number=MATCH,
                printable="Y",
                price=0x55667788,
            ),
            bytes.fromhex("0102030405060708 11223344 1112131415161718 59 55667788"),
            36,
        ),
        (
            "X",
            dict(order_id=ORDER_ID, shares=0x11223344),
            bytes.fromhex("0102030405060708 11223344"),
            23,
        ),
        ("D", dict(order_id=ORDER_ID), bytes.fromhex("0102030405060708"), 19),
        (
            "U",
            dict(order_id=ORDER_ID, new_order_id=MATCH, shares=0x11223344, price=0x55667788),
            bytes.fromhex("0102030405060708 1112131415161718 11223344 55667788"),
            35,
        ),
        ("P", {**ADD, "match_number": MATCH}, ADD_BYTES + bytes.fromhex("1112131415161718"), 44),
    ],
)
def test_exact_wire_layout(kind, fields, body, length):
    encoded = encode_event(
        dict(
            type=kind,
            stock_locate=0x1234,
            tracking_number=0x5678,
            timestamp=0x010203040506,
            **fields,
        )
    )
    assert encoded == bytes([0, length]) + kind.encode() + HEADER + body
    assert len(encoded) == length + 2


@pytest.mark.parametrize("value", [-1, 2**32, 1.5, True, "100"])
def test_reject_invalid_price(value):
    with pytest.raises(ValueError, match="price"):
        encode_event(dict(type="A", **{**ADD, "price": value}))


@pytest.mark.parametrize(
    ("field", "value"),
    [
        ("stock_locate", 2**16),
        ("tracking_number", -1),
        ("timestamp", 2**48),
        ("order_id", 2**64),
        ("shares", -1),
        ("stock", "TOOLONG12X"),
        ("stock", "é"),
        ("stock", "\n"),
        ("stock", ""),
        ("side", "BUY"),
    ],
)
def test_reject_invalid_fields(field, value):
    with pytest.raises(ValueError, match=field):
        encode_event({"type": "A", **ADD, field: value})


def test_full_width_unsigned_boundaries():
    encoded = encode_event(
        dict(
            type="A",
            stock_locate=65535,
            tracking_number=65535,
            timestamp=2**48 - 1,
            order_id=2**64 - 1,
            side="S",
            shares=2**32 - 1,
            stock="ABCDEFGH",
            price=2**32 - 1,
        )
    )
    assert encoded == b"\x00\x24A" + b"\xff" * 18 + b"S" + b"\xff" * 4 + b"ABCDEFGH" + b"\xff" * 4


@pytest.mark.parametrize(
    "event",
    [{"type": "Z"}, {"type": []}, {}, {"type": "A"}, {"type": "S", "event_code": "O", "typo": 1}],
)
def test_reject_unknown_or_missing_fields(event):
    with pytest.raises(ValueError):
        encode_event(event)


def test_stream_and_terminator():
    events = [{"type": "S", "event_code": "O"}, {"type": "S", "event_code": "C"}]
    expected = b"\x00\x0cS" + b"\0" * 10 + b"O\x00\x0cS" + b"\0" * 10 + b"C"
    assert encode_events(iter(events)) == expected
    assert encode_events(events, end_session=True) == expected + b"\0\0"
    assert encode_events([]) == b""
    assert encode_events([], end_session=True) == b"\0\0"


def test_raw_unknown_message():
    assert frame(b"?abc") == b"\0\x04?abc"
    assert frame(b"Z" * 65535) == b"\xff\xff" + b"Z" * 65535


@pytest.mark.parametrize(
    "payload", [b"", b"a" * 65536, "text"], ids=["empty", "oversized", "not-bytes"]
)
def test_reject_invalid_raw_frame(payload):
    with pytest.raises(ValueError, match="payload"):
        frame(payload)
