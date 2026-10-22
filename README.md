# Tachyon

A C++20 trading-core project with an ITCH feed handler for reconstructing exchange
books and a separate matching engine for processing incoming orders. ITCH replay
rebuilds the exchange's published book; it does not match orders.

The parser decodes ITCH messages from mapped files, with a Python reference
decoder and real/synthetic fixture checks. The book builder reconstructs one
book per symbol and is checked against a slow reference book on 10,000 random
streams and on a full trading day. Matching, the threaded pipeline, and
benchmarks belong to later milestones. There are no performance results yet.

See [build and test instructions](docs/build.md),
[data tooling](docs/data.md), [parser design](docs/parser.md), and
[book reconstruction](docs/book.md).
The final architecture and benchmark documentation
will accompany the completed implementation.
