# Benchmarks

Every number below comes from a file in [`results/m6/`](../results/m6/), produced by
`python bench/run_benchmarks.py --build build/release --output results/m6` (rerun one
file with `--only <name>`). Each benchmark ran at least five times; the tables give
the median and, in brackets, the minimum and maximum across runs. Latency rows give
the median across runs of each percentile. Plots come from
`python python/latency_plots.py --results results/m6`.

## Machine and method

| | |
|---|---|
| CPU | AMD Ryzen 9 8940HX, 16 cores / 32 threads, two 8-core CCDs |
| RAM | 16 GB |
| OS | Windows 11 Home build 26300, Balanced power plan |
| Compiler | MSVC 19.44, release preset (`/O2 /Ob2 /DNDEBUG`), invariant checks compiled out |
| Data | NASDAQ ITCH 5.0, 30 January 2019, 368,366,634 messages (11.2 GB) |

- Single-threaded benchmarks are pinned to logical CPU 2; the pipeline pins its
  parser, book and publisher threads to CPUs 2, 4 and 6 (separate physical cores).
- Turbo boost was **not** disabled and the machine is a laptop on the Balanced power
  plan, which parks idle cores. Both add noise; the min/max columns show how much.
- The TSC is invariant (CPUID 0x80000007), runs at 2.4 ticks/ns, but on this CPU it
  advances in steps of about 24 ticks, so **latencies are quantized to 10 ns**. The
  cost of the measurement itself (two back-to-back reads) is p50 10 ns, p99 20 ns.
- **`perf stat` was not run.** It does not exist on Windows, and no equivalent
  hardware-counter tool is installed here. The explanations below that depend on
  cache behavior are therefore hypotheses, marked as such.

## Results

| Benchmark | Result | Spec ballpark |
|---|---|---|
| Parse only, full day ([parse_throughput](../results/m6/parse_throughput.json)) | 15,805,700 msgs/s [14,539,900..16,268,700] | tens of millions/s: **below** |
| Parse only, first 50M messages, in page cache ([parse_prefix_throughput](../results/m6/parse_prefix_throughput.json)) | 30,453,200 msgs/s [26,069,600..49,373,400] | tens of millions/s: met |
| Parse + book build, full day ([book_throughput](../results/m6/book_throughput.json)) | 3,074,180 msgs/s [2,785,600..3,134,920] | several million/s: met |
| Matching engine, 20M synthetic orders ([matching_engine](../results/m6/matching_engine.json)) | 4,936,580 orders/s [4,909,700..5,008,600]; p50 70 ns, p99 323 ns | report honestly |

**Book update latency per message, full day** ([book_op_latency](../results/m6/book_op_latency.json), [plot](../results/m6/book_op_latency_cdf.png)), in ns:

| ITCH type | Messages | p50 | p90 | p99 | p99.9 |
|---|---|---|---|---|---|
| A add | 162,970,455 | 191 | 311 | 575 | 2,431 |
| F add with MPID | 1,725,898 | 241 | 511 | 1,471 | 6,335 |
| E execute | 8,096,995 | 191 | 363 | 527 | 855 |
| C execute with price | 158,886 | 100 | 303 | 471 | 703 |
| X cancel | 4,669,874 | 110 | 291 | 423 | 607 |
| D delete | 158,273,361 | 231 | 423 | 631 | 975 |
| U replace | 27,222,746 | 463 | 927 | 1,887 | 4,031 |

The spec's ballpark is "p50 well under 100 ns". **That is not met**: adds and deletes
sit around 190 to 230 ns at the median.

**Pipeline latency**, parse timestamp to book-updated timestamp
([pipeline_latency](../results/m6/pipeline_latency.json), [plot](../results/m6/pipeline_latency_cdf.png)), in ns:

| Input | Achieved msgs/s | p50 | p90 | p99 | p99.9 |
|---|---|---|---|---|---|
| Paced at 1M msgs/s (first 50M messages) | 999,960 [988,128..999,963] | 411 | 743 | 15,615 | 102,760,447 |
| Paced at 2M msgs/s (first 50M messages) | 1,999,830 [1,999,830..1,999,850] | 471 | 2,911 | 24,641,535 | 123,731,967 |
| Unpaced, full day | 2,343,370 [775,238..2,984,850] | 22,020,095 | 29,097,983 | 39,321,599 | 61,865,983 |

**Components** ([micro](../results/m6/micro.json)):

| Benchmark | Custom | Alternative |
|---|---|---|
| Id map insert, 1M ids (ms per batch) | `OrderMap` 29.2 [24.9..37.8] | `std::unordered_map` 404.4 [386.4..415.3] |
| Id map lookup, hit (ns) | 8.3 [7.7..9.6] | 55.9 [50.2..58.8] |
| Id map lookup, miss (ns) | 27.6 [26.9..29.7] | 49.6 [49.1..56.9] |
| Id map churn at 1M live: delete random + insert new (ns) | 244.3 [193.3..253.4] | 963.4 [897.7..1,004.3] |
| Price levels, ITCH-shaped churn (ns) | sorted vector 43.0 [40.2..43.5] | `std::map` 92.1 [85.7..103.5] |
| SPSC ring, 10M items, consumer on the same CCD (ms) | padded 99.8 [91.4..112.5] | unpadded 99.9 [96.7..103.1] |
| SPSC ring, 10M items, consumer on the other CCD (ms) | padded 204.1 [198.1..1,379.7] | unpadded 317.9 [289.5..345.0] |

Both maps were reserved for 1M entries before timing, so neither pays for growth
during the lookup and churn benchmarks.

## What the numbers mean

- **Parse only.** The parser decodes about 30 million messages per second when the
  input is already in memory (the 1.5 GB prefix), within the spec's ballpark. Over
  the whole 11.2 GB day on a 16 GB machine it reaches about half that. The likely
  cause is the cost of faulting in the memory mapping (about 2.7 million 4 KB pages)
  and reading pages the cache could not keep, not the decoding. Without hardware
  counters this is an inference from the two results, not a measurement.
- **Book updates miss the 100 ns ballpark.** A full day keeps up to 1.7 million
  orders live, so the id map, the order pool and the level vectors are far larger
  than the L2 cache, and an add or delete typically touches several lines that are
  not cached: the id map slot, the order, its level, and the level's neighbors. The
  matching-engine benchmark, whose working set is small, does comparable work at a
  70 ns median. That contrast is consistent with cache misses being the main cost,
  but it was not confirmed with counters. The 10 ns timer granularity and 10 ns
  measurement overhead are included in every sample.
- **Replace (U)** is the slowest operation at about 460 ns at the median because it
  is a full delete plus a full add, including two id-map updates and usually a
  different level.
- **Pipeline, unpaced.** Replaying from memory, the parser outruns the book thread,
  so the 65,536-slot ring stays full and every message waits behind the whole queue.
  The ~22 ms median is that queueing delay (65,536 slots at about 2.3 million
  messages per second is about 28 ms), not processing time. One of the five full-day
  runs ran at 775,238 msgs/s, which is why the throughput range is wide.
- **Pipeline, paced.** At 1M and 2M messages per second, the median is about 0.4 to
  0.5 microseconds from parse to book update. The tail is poor: p99.9 is above
  100 ms at both rates and p99 is 24.6 ms at 2M/s. The likely explanation is that the
  book thread occasionally stalls; once it does, the queue fills at the input rate
  and every queued message inherits the stall. This machine has no isolated cores, runs on a Balanced power
  plan with core parking, and services interrupts on any core, which is the likely
  source of the stalls. A production setup would isolate the pipeline's cores and
  disable power management. That was not possible here, so these tails are reported
  as measured.
- **Id map.** The custom map is about 6.7x faster than `std::unordered_map` for
  lookups that hit, 1.8x for misses and 3.9x under steady-state churn. Open
  addressing keeps a lookup in one or two adjacent slots, while `std::unordered_map`
  follows a pointer to a separately allocated node.
- **Price levels.** The sorted vector searched from the best price is about 2.1x
  faster than `std::map` for level churn concentrated near the touch, which is why
  M3 uses it.
- **Ring padding.** With both threads on the same CCD, padding makes no measurable
  difference: the two cores share an L3, so moving a contended line between them is
  cheap. With the consumer on the other CCD, the padded ring is about 1.6x faster at
  the median (204 ms against 318 ms), because without padding every push and pop
  bounces the shared index line across the die interconnect. Two of the five padded
  cross-CCD runs took about 1.3 s; the likely cause is core parking under the
  Balanced plan (the consumer's core waking up mid-run). They are kept in the data
  rather than discarded.
