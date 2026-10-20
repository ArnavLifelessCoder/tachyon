# Tachyon

A C++20 trading-core project with an ITCH feed handler for reconstructing exchange
books and a separate matching engine for processing incoming orders. ITCH replay
rebuilds the exchange's published book; it does not match orders.

M1 provides the build skeleton, test infrastructure, and synthetic data tools.
The parser, book, matching engine, threaded pipeline, and benchmarks belong to
later milestones. There are no performance results yet.

See [build and test instructions](docs/build.md) and
[data tooling](docs/data.md). The final architecture and benchmark documentation
will accompany the completed implementation.
