# ITCH parser

The decoder uses the field offsets in Nasdaq's
[ITCH 5.0 specification](https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHSpecification_5.0.pdf),
sections 4.1, 4.2.1, 4.3, 4.4, and 4.5.1, with
[BinaryFILE framing](https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/binaryfile.pdf).

## Interface and ownership

`include/me/itch_parser.hpp` exposes
`me::itch::parse(std::span<const std::byte>, Handler&, Stats* = nullptr)`.
The handler supplies an overloaded `on(const T&)` for each of `SystemEvent`,
`Directory`, `Add`, `AddMpid`, `Execute`, `ExecutePrice`, `Cancel`, `Delete`,
`Replace`, and `Trade`, plus `on_unknown(unsigned char, std::span<const std::byte>)`.
The template permits direct, inlined dispatch without a virtual interface.

Each decoded message is a trivial, standard-layout value with a common `Header`.
All fields are decoded, including every directory field. Fixed-width text stays
space-padded in `std::array<char, N>`. Integers retain their full unsigned widths;
timestamps use a 64-bit container for the 48-bit wire value, and prices remain
integers with four implied decimal places. Loads use `memcpy` and compiler
byte-swap intrinsics, so the source need not be aligned and strict aliasing is
preserved. No intermediate message buffer or heap allocation is required.

Callbacks receive temporary decoded values; copy a value if it must outlive the
callback. Unknown payload spans refer to the original input and must not outlive
that input. The parser never owns or changes input bytes. Handler allocation and
exceptions are the caller's responsibility.

## Framing, errors, and statistics

The parser accepts an empty input, a fragment ending exactly at a message
boundary, or one session with a final zero-length terminator. Bytes following a
terminator are rejected. Known types must have their exact specified lengths.
Other types are skipped by the prefix, even if their opaque payload is shorter
than an ITCH common header.

`ParseError` reports an error code and the byte offset of the failing frame's
length prefix. Codes distinguish truncated prefixes, truncated payloads,
incorrect known-message lengths, and trailing bytes after the terminator.
No callback runs for an invalid frame. Already completed callbacks are not
rolled back. This decoder checks structure, not order lifecycles, permissible
enum values, or trading rules.

Optional `Stats` is reset on entry. It contains arrays indexed by every possible
type byte for total counts and skipped counts, plus total messages, consumed
bytes, and whether a terminator was seen. On failure it describes only completed
callbacks. A terminator consumes two bytes but is not a message.

## Files and gzip

`src/platform/` implements read-only mappings using Windows file mappings and
POSIX `mmap`. Mapping ownership is scoped and noncopyable. Empty files produce an
empty span. Files must remain unchanged while mapped; Windows mappings deny
concurrent writers, while POSIX callers must enforce this externally.

The `itch_parse` executable accepts a raw file and prints JSON counts. Add
`--events` for decoded fields; this diagnostic mode buffers JSON and is not a
benchmark. Windows uses a wide-character entry point for Unicode filesystem
paths. All interfaces use `std::filesystem::path`.

```powershell
./build/debug/itch_parse.exe tests/fixtures/01302019.AAPL-MSFT-SPY.itch
./build/debug/itch_parse.exe tests/fixtures/synthetic.itch --events
./.venv/Scripts/python.exe python/ref_parser.py tests/fixtures/01302019.AAPL-MSFT-SPY.itch
./.venv/Scripts/python.exe python/parse_file.py data/01302019.NASDAQ_ITCH50.gz --parser build/debug/itch_parse.exe
```

The Python launcher handles `.gz` input by fully decompressing and checking gzip
integrity before starting the C++ process. It writes to a unique directory under
the repository's ignored `.tmp/`, maps the resulting raw file in C++, and removes
the scratch directory after success or failure. Decompression is outside the
parse loop. Raw input goes directly to the mapped executable. The launcher has
a 120-second child-process timeout; invoke the native executable on a prepared
raw file for longer runs.

## Validation

Build-time cases use `itch_writer.py` to create bytes for every known message
type. C++ assertions check every field at zero, patterned values, and unsigned
maximums. Every cut inside those messages is tested with an allocation ending
at the cut and an unaligned starting address. The randomized GoogleTest mutates
valid seed streams from the synthetic fixture and random frame-aligned slices
of the real book fixture. Trials flip bytes, rewrite lengths (minus/plus one,
zero, 65535, or random), replace type bytes, truncate, insert, or delete bytes.
Mutation locations include the first frame. Each input is copied into an
exact-size unaligned allocation before parsing. Coverage assertions require
every known type to be decoded and more than 75% of trials to decode at least
one frame. Unknown-type tests also include the maximum frame length.

`python/ref_parser.py` uses its own `struct.unpack` formats and imports no writer
or C++ layouts. Pytest compares complete decoded events on 200 seeded random streams,
compares malformed-input codes and offsets, and requires exact counts from C++,
the reference parser, and each fixture manifest. The AAPL/MSFT/SPY book fixture
is mandatory and covers all eight book types. Its cut ends shortly after the
open; it is not representative of regular-session book shape. Full-day
crossed-book analysis belongs to M3. A second differential test writes 200 mutated
fixture streams and a seed/hash manifest into its pytest temporary directory.
It compares full event output for accepted inputs and `{error, offset}` for
rejected inputs. Failed assertions include the case seed and saved input path.

A separate allocation probe replaces ordinary `new`/`new[]` and runs the real
book fixture through a counting handler. Its Python runner reads the fixture
filename and expected message count from the manifest before starting the native
probe. Mapping and diagnostics happen outside the measured region.
No parsing performance claims are made.

To repeat a seed (use the corresponding executable path on Linux):

```powershell
./build/debug/parser_tests.exe --gtest_filter=Parser.MutatedCorpusExactAllocationAndUnalignedInput --seed=1234
./.venv/Scripts/python.exe -m pytest tests/python/test_parser_integration.py --parser build/debug/itch_parse.exe --seed=1234 --basetemp .tmp/repro
```

CTest runs these checks in both debug and ASan configurations. TSan begins with
the threaded components in M4.
