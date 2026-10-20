# ITCH data tooling

The writer follows the field offsets and sizes in Nasdaq's
[ITCH 5.0 specification, March 6, 2015](https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHSpecification_5.0.pdf),
sections 3, 4.1, 4.2.1, 4.3, 4.4, and 4.5.1. File framing follows
[BinaryFILE 1.00](https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/binaryfile.pdf).
Numeric fields use big-endian encoding. Text is printable ASCII, padded on the
right with spaces. Prices stay unsigned integer units of 0.0001.

## Synthetic events

`python/itch_writer.py` accepts a JSON list of event objects. Each event has
`type` and its message-specific fields. Common `stock_locate`, `tracking_number`,
and `timestamp` default to zero. Directory fields have test-oriented defaults
documented in `SCHEMAS`; stock is required. Other message-specific fields are
required. Unknown keys, out-of-range numbers, floats, and invalid text are errors.
The writer validates representation, not exchange semantics or order lifecycles.

```sh
python python/itch_writer.py tests/fixtures/synthetic.json new-fixture.itch --end-session
```

The optional end-session flag appends the BinaryFILE zero-length terminator.
Without it, the output is a stream fragment. `encode_event` and `encode_events`
provide the same functionality for tests; `frame(payload)` frames raw bytes
for unknown-message and malformed-message test cases.

The committed `tests/fixtures/synthetic.itch` is generated from the adjacent JSON
with `--end-session`. It contains all ten required message types, two symbols,
partial and full executions, a cancel, a replacement with a new ID, deletions,
and a non-cross trade. The generation command's byte count, SHA-256, and per-type
counts are recorded in `tests/fixtures/synthetic.manifest.json`. Tests reproduce the file exactly
and check framing against independent expected lengths.

## Cut a fixture

```sh
python scripts/make_fixture.py input.itch.gz cut.itch --messages 1000
```

The cutter streams raw or gzip input, copies exactly the first requested number
of complete messages, and produces raw BinaryFILE framing. It rejects early EOF,
truncated frames, and a session terminator before the requested count. Output
must be strictly below 5 MB. It does not append an end-of-session marker because
a cut is not a complete trading session. It never overwrites an existing output.

## Download a real day

The downloader is invoked explicitly; CI does not download full-day files:

```sh
python scripts/download_itch.py 2019-01-30
```

It streams Nasdaq's compressed file into `data/01302019.NASDAQ_ITCH50.gz.part`,
checks its size against Content-Length, and publishes the final path only after
validation. If the server omits Content-Length, pass a separately known compressed
byte count using `--expected-size`. If both values are present they must agree.
Successful output includes the measured byte count and SHA-256. The hash is an
audit record, not an independently verified provider checksum.

Publishing first tries a same-directory hard link. If hard links are unavailable
and the destination is absent, it falls back to `os.replace`. Avoid concurrent
writers to that destination when using the fallback: the absence check and rename
are separate operations. If publication fails, the completed `.part` is retained
for recovery. Existing final or partial files cause an error; interrupted writes
remove the incomplete partial file created by this invocation.
Tests mock the download response and never download market data.

Native C++ file mapping, synchronization, rename, and affinity adapters will live
under `src/platform/` when their consuming components are introduced.
