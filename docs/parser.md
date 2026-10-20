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
a 20-second child-process timeout; invoke the native executable on a prepared
raw file for longer runs.

### Batch diagnostics

`itch_parse --batch [--events]` reads one UTF-8 filesystem path per line from
standard input. Paths can contain spaces and Unicode but not line breaks.
Each input produces one JSON line, in the same order:
`{"status":0,"result":{...}}`. Status 0 contains the normal counts or events,
status 2 contains a parse error and offset, and status 1 contains
`{"error":"io_error","message":...}`. A failed input does not stop later inputs.
The process exits 0 after consuming the list, including a list containing failed
inputs; a protocol, input-stream, or output-stream failure exits 1. An empty
list emits no output. Raw files are mapped and unmapped separately.

`python.parse_file.run_parser_batch` sends the list through standard input, so
the Windows command-line length limit does not constrain corpus size. It captures
stdout and stderr in repository-local temporary files and applies one 20-second
deadline to the entire invocation. On timeout it preserves the captured output
on the exception, reports the last announced input path (including its seed), and
does not retry. A timeout before any input announcement lists the batch inputs.

The executable flushes `itch_parse: main entered` to stderr as its first action
in `main`/`wmain`, and `itch_parse: main returning status=N` immediately before
return, after local argument storage has been destroyed. Batch processing also
flushes `itch_parse: input index=N` before each mapping. These diagnostic markers
are separate from JSON stdout.

### MSVC ASan launch investigation

`scripts/probe_parser_launches.py` repeatedly launches the instrumented executable
on one unchanged fixture and stops at the first timeout or failure, without
retrying. The captured run is in `results/m2/asan-launch-probe.json`: the first
52 launches completed, and launch 53 timed out at the 20-second deadline on the
484-byte synthetic fixture. Its stderr contained both `main entered` and
`main returning status=0`. Neither marker was missing. This observed stall is
after successful parsing and the final marker before return from `wmain`, rather
than inside the parser or mapping lifetime. The exact runtime/OS exit mechanism
is not established by these markers.

The first full acceptance sequence subsequently stopped during
`mapping_tests --gtest_list_tests`: it printed its three test names, then
discovery timed out. That failed sequence is retained in
`results/m2/windows-suite-runs-before-native-batch.json`. CTest now registers
each entire GoogleTest executable directly, removing discovery and per-case
process launches. All 38 native cases still run, in three processes. Together
with the allocation probe and Python suite, there are five CTest entries.
A regression checks the configured CTest commands to prevent a return to
per-case launches or filters. Native suites have a 20-second timeout.

With native launches grouped, a later field-differential batch reached input
198 of 200 before its 20-second deadline, without a return marker. That sequence
is retained in `results/m2/windows-suite-runs-before-json-buffer.json`. Diagnostic
event formatting now appends to a string and uses `std::to_chars` for integers,
avoiding a formatted stream operation for every field. The deadline and all
200 seeds with 500 known messages each are unchanged.

The focused validation then captured another exit stall on a three-byte
malformed input: both markers were present, with return status 2
(`results/m2/asan-error-exit-stall.json`). Parameterized fixture-count,
binary-boundary, and malformed-input checks now share one batch per group.
Their individual pytest cases and reference assertions are retained.

Batching reduces the number of launches exposed to this MSVC ASan exit stall;
it does not claim to repair the sanitizer runtime. A future timeout still fails
the test immediately with captured markers and input context. To characterize
a recurrence, run from a Visual Studio developer shell:

```powershell
./.venv/Scripts/python.exe scripts/probe_parser_launches.py --parser build/asan/itch_parse.exe --launches 1000 --output results/m2/asan-launch-probe.json
```

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
Each differential test writes all 200 inputs first and invokes batch mode once.
The field test uses a distinct `seed-N.itch` file for every seed, avoiding
rewrites of paths recently mapped by a Windows child process.

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

On Windows, `./scripts/check_parser_suites.ps1` runs the complete debug and ASan
suites three consecutive times each. It stops on the first failure, preserves
each verbose log under `.local/review/`, and records CTest and total invocation
times in `results/m2/windows-suite-runs.json`. Each preset is configured and built
once before its three test runs.
