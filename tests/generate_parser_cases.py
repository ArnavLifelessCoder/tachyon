"""Generate C++ field assertions from the synthetic wire writer at build time."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from python.itch_writer import HEADER, SCHEMAS, encode_event  # noqa: E402

TYPES = dict(
    zip(
        "SRAFECXDUP",
        (
            "SystemEvent Directory Add AddMpid Execute ExecutePrice Cancel Delete Replace Trade"
        ).split(),
        strict=True,
    )
)


def generate(destination):
    lines = ["// Wire cases produced by tests/generate_parser_cases.py."]
    for kind, cpp in TYPES.items():
        for mode in ("Pattern", "Maximum", "Zero"):
            event = {"type": kind}
            assertions = []
            for index, field in enumerate((*HEADER, *SCHEMAS[kind])):
                member = ("header." if index < 3 else "") + field.name
                if field.kind == "uint":
                    value = int.from_bytes(bytes(range(1, field.width + 1)), "big")
                    if mode == "Maximum":
                        value = (1 << (8 * field.width)) - 1
                    elif mode == "Zero":
                        value = 0
                    assertions.append(f"EXPECT_EQ(value.{member}, {value}ULL);")
                else:
                    value = chr(65 + index % 26)
                    if field.width == 1:
                        assertions.append(f"EXPECT_EQ(value.{member}, '{value}');")
                    else:
                        assertions.append(
                            f"EXPECT_EQ(std::string(value.{member}.data(), {field.width}), "
                            f'"{value.ljust(field.width)}");'
                        )
                event[field.name] = value
            encoded = encode_event(event)
            values = ",".join(str(x) for x in encoded)
            lines += [
                f"TEST(ParserFields, {cpp}{mode}) {{",
                f"const std::array<unsigned char, {len(encoded)}> data = {{{values}}};",
                f"Capture<me::itch::{cpp}> handler;",
                "me::itch::Stats stats;",
                "const auto bytes = std::as_bytes(std::span(data));",
                "me::itch::parse(bytes, handler, &stats);",
                "ASSERT_TRUE(handler.value.has_value());",
                "const auto& value = *handler.value;",
                *assertions,
                "EXPECT_EQ(stats.messages, 1U);",
                f"EXPECT_EQ(stats.counts['{kind}'], 1U);",
                "EXPECT_EQ(stats.bytes, data.size());",
                "EXPECT_FALSE(stats.terminated);",
                "for (std::size_t cut = 1; cut < bytes.size(); ++cut) {",
                "SCOPED_TRACE(cut);",
                f"Capture<me::itch::{cpp}> truncated;",
                "EXPECT_THROW(me::itch::parse(bytes.first(cut), truncated), me::itch::ParseError);",
                "EXPECT_FALSE(truncated.value.has_value());",
                "}",
                "}",
            ]
    destination.write_text("\n".join(lines) + "\n", encoding="utf-8")


if __name__ == "__main__":
    generate(Path(sys.argv[1]))
