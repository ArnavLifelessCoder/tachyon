# Tachyon

A C++20 trading-core project with an ITCH feed handler for reconstructing exchange
books and a separate matching engine for processing incoming orders. ITCH replay
rebuilds the exchange's published book; it does not match orders.

The parser decodes ITCH messages from mapped files, with a Python reference
decoder and real/synthetic fixture checks. The book builder reconstructs one
book per symbol and is checked against a slow reference book on 10,000 random
streams and on a full trading day. A three-thread pipeline (parser, book,
publisher) connected by lock-free SPSC rings produces the same books and BBO
sequence as the single-threaded run. A price-time matching engine is checked
against a simple Python matcher on 1,000 random order streams. Measured
throughput and latency, with the machine and method, are in
[benchmarks](docs/benchmarks.md).

See [build and test instructions](docs/build.md),
[data tooling](docs/data.md), [parser design](docs/parser.md),
[book reconstruction](docs/book.md), [threaded pipeline](docs/pipeline.md),
[matching engine](docs/matching.md), and [benchmarks](docs/benchmarks.md).
The final architecture and benchmark documentation
will accompany the completed implementation.
