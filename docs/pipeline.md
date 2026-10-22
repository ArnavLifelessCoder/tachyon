# Threaded pipeline

```
[mapped file] -> parser thread -> SPSC ring -> book thread -> SPSC ring -> publisher thread
                                                                           (BBO, trades, latency)
```

`src/pipeline.cpp` runs three threads, each optionally pinned to its own logical CPU
(`SetThreadAffinityMask` on Windows, `pthread_setaffinity_np` on Linux) and each
busy-spinning with `pause` when its ring is empty or full. Nothing on the hot path
takes a lock or allocates: rings, books, pools and the histogram are sized before
the threads start.

- **Parser thread** decodes the mapped file with the M2 parser and pushes each
  message, stamped with `rdtsc`, as a `std::variant` of the ITCH POD structs.
- **Book thread** applies each message to the M3 `BookBuilder`, stamps `rdtsc`
  again, and pushes an `Update` (the touched symbol's best bid/offer and both
  timestamps) for every book-changing message and trade.
- **Publisher thread** keeps the last BBO per symbol, hashes every BBO change into a
  running digest, and records parse-to-book latency in the histogram.

`run_single` runs the same three stages inline on one thread. It is the reference
the threaded run must match.

## SPSC ring (`include/me/spsc_ring.hpp`)

Power-of-two capacity, free-running 64-bit head and tail indices on separate
64-byte cache lines, and a private cached copy of the other side's index on each
side's line. The producer refreshes its copy of `head` only when the ring looks
full, and the consumer refreshes `tail` only when it looks empty, so in steady state
each side writes only its own line.

Memory ordering, as justified in the header:

- The producer writes the slot and then `tail.store(release)`. The consumer's
  `tail.load(acquire)` that sees the new value also sees the slot.
- The consumer reads the slot and then `head.store(release)`. The producer's
  `head.load(acquire)` that sees it is ordered after that read, so a slot is never
  overwritten while it is being read.
- Each side reads its own index relaxed, because only it writes that index.

`std::hardware_destructive_interference_size` is not used: GCC warns under
`-Werror` that its value depends on `-mtune`. The ring uses 64, the line size of
every x86-64 part targeted. M6 benchmarks the padding against an unpadded ring.

## Timestamps

`rdtsc` is read via the compiler intrinsic and converted to nanoseconds using a
ticks-per-nanosecond rate measured against `steady_clock` over 50 ms at startup.
TSC differences between threads are only meaningful with an invariant TSC, so the
pipeline checks CPUID leaf 0x80000007 EDX bit 8 (what Linux reports as
`constant_tsc` plus `nonstop_tsc`) and records the answer with every result.

## Latency histogram (`include/me/histogram.hpp`)

Log-linear, HdrHistogram style: values below 64 are exact, and every power of two
above that is split into 64 sub-buckets, so a reported value is within 1/64 (1.6%)
above the true one. It reports p50, p90, p99, p99.9 and max from a fixed
3,776-bucket array; `record()` never allocates. Tests check the error bound against
exact sorted percentiles on a long-tailed distribution.

## Checks

- `SpscRing.StressOrderAndNoLoss`: 100,000,000 items through a 1,024-slot ring
  between two threads; every item must arrive once and in order. A second stress
  test sends 32-byte items and checks none is ever read torn.
- `Pipeline.ThreadedMatchesSingleThreadedOnRealFixture`: the AAPL/MSFT/SPY fixture
  through both paths, with a 4-slot ring (constant backpressure) and a 65,536-slot
  ring. Book digests at every 10,000 messages and at the end, the BBO change
  digest, message and update counts must all be equal.
- The concurrency suites run under ThreadSanitizer in Linux CI (GCC and Clang).
  Windows has no TSan; see `docs/build.md` for the Windows sanitizer policy.

## Full trading day

`python scripts/pipeline_day.py --pipeline build/release/pipeline_replay.exe --output results/m4/full_day_pipeline.json`
runs all 368,366,634 messages of 30 January 2019 through both paths (threads pinned
to logical CPUs 2, 4 and 6, which are separate physical cores on the recorded
machine). From [`results/m4/full_day_pipeline.json`](../results/m4/full_day_pipeline.json):

- **Books identical.** A full day ends with every book empty, so the final digest
  alone proves little. Both runs also record a digest of every resting order of
  every book each 25,000,000 messages; all 14 checkpoints match.
- **Published BBO sequence identical**: the same 174,890,436 BBO changes in the
  same order (equal running digest), from 364,444,399 updates.
- Zero unknown order references in both runs. The CPU reports an invariant TSC.

The same file records throughput and parse-to-book latency for both runs. The
threaded latency is dominated by queueing, not processing: replaying from memory,
the parser outruns the book thread, the 65,536-slot ring stays full, and each
message waits behind the whole queue (65,536 slots at about 2.5 million messages
per second is about 26 ms, which matches the recorded median). The single-threaded
figure is the cost of applying one message with nothing queued. Latency under a
controlled input rate, which is what a live feed looks like, is measured in M6.
