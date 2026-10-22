"""Run every benchmark at least five times and write results/m6 with machine info.

Throughput is reported as the median with min and max across runs. Latency is
reported per run (p50/p90/p99/p99.9/max from the allocation-free histogram) and
summarized as the median of each percentile across runs; the median run's bucket
counts are kept for the CDF plots.

Usage (from a VS developer shell on Windows, or any shell on Linux):
  python bench/run_benchmarks.py --build build/release --output results/m6
"""

import argparse
import json
import statistics
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from scripts.machine_info import collect  # noqa: E402
from scripts.replay_day import MANIFEST, raw_day  # noqa: E402

PERCENTILES = ("p50", "p90", "p99", "p999", "max")


def run_json(command, timeout=7200):
    result = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    if result.returncode != 0:
        raise RuntimeError(f"{command} failed ({result.returncode}): {result.stderr}")
    return json.loads(result.stdout)


def spread(values):
    return {
        "median": statistics.median(values),
        "min": min(values),
        "max": max(values),
        "runs": values,
    }


def latency_summary(histograms):
    """Median and range of each percentile across runs, plus the median run's buckets."""
    summary = {p: spread([h[p] for h in histograms]) for p in PERCENTILES}
    by_p50 = sorted(histograms, key=lambda h: h["p50"])
    summary["count_per_run"] = histograms[0]["count"]
    summary["median_run_buckets"] = by_p50[len(by_p50) // 2]["buckets"]
    return summary


def prefix_file(raw: Path, messages: int) -> Path:
    """First `messages` frames of the raw day, for paced pipeline runs."""
    out = raw.with_name(f"{raw.name}.first{messages}")
    if out.exists():
        return out
    with raw.open("rb") as source:
        offset = 0
        for _ in range(messages):
            source.seek(offset)
            offset += 2 + int.from_bytes(source.read(2), "big")
        source.seek(0)
        remaining = offset
        with out.open("wb") as target:
            while remaining:
                chunk = source.read(min(remaining, 64 << 20))
                target.write(chunk)
                remaining -= len(chunk)
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=ROOT / "build/release")
    parser.add_argument("--source", type=Path, default=ROOT / "data/01302019.NASDAQ_ITCH50.gz")
    parser.add_argument("--output", type=Path, default=ROOT / "results/m6")
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--cpus", nargs=3, type=int, default=[2, 4, 6], metavar="CPU")
    parser.add_argument("--paced-messages", type=int, default=50_000_000)
    parser.add_argument("--rates", nargs="+", type=float, default=[1e6, 2e6])
    parser.add_argument("--match-orders", type=int, default=20_000_000)
    parser.add_argument("--flags", default="release preset")
    parser.add_argument("--only", nargs="+", help="rerun only these result files, e.g. micro")
    args = parser.parse_args()
    if args.runs < 5:
        sys.exit("convention: at least five runs per benchmark")

    exe_suffix = ".exe" if sys.platform == "win32" else ""
    replay = str((args.build / f"bench_replay{exe_suffix}").resolve())
    micro = str((args.build / f"bench_micro{exe_suffix}").resolve())
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    raw = raw_day(args.source, manifest["source_uncompressed_bytes"])
    prefix = prefix_file(raw, args.paced_messages)
    cpus = list(map(str, args.cpus))
    args.output.mkdir(parents=True, exist_ok=True)
    machine = collect(args.flags)
    common = {
        "machine": machine,
        "date": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "source_file": args.source.name,
        "source_sha256": manifest["source_sha256"],
        "runs": args.runs,
        "pinning": f"single-threaded modes on CPU {args.cpus[0]}; pipeline on CPUs {cpus}",
        "turbo": "not disabled (no supported way on this machine); power plan recorded",
    }

    def write(name, record):
        path = args.output / f"{name}.json"
        path.write_text(json.dumps({**common, **record}, indent=2) + "\n", encoding="utf-8")
        print(f"wrote {path}", flush=True)

    def throughput(mode, source=None, name=None, what="the full day"):
        source = source or raw
        run_json([replay, mode, str(source)])  # warm the page cache; not recorded
        runs = [run_json([replay, mode, str(source)]) for _ in range(args.runs)]
        write(
            name or f"{mode}_throughput",
            {
                "benchmark": f"{mode} {what}, single thread",
                "command": f"bench_replay {mode} <{what}>",
                "messages": runs[0]["messages"],
                "messages_per_second": spread([r["messages_per_second"] for r in runs]),
                "seconds": spread([r["seconds"] for r in runs]),
            },
        )

    def op_latency():
        runs = [run_json([replay, "ops", str(raw)]) for _ in range(args.runs)]
        write(
            "book_op_latency",
            {
                "benchmark": "per-message book update latency over the full day, by ITCH type (ns)",
                "command": "bench_replay ops <raw day>",
                "note": "each sample includes one rdtsc pair; timer_overhead_ns measures that pair",
                "timer_overhead_ns": latency_summary([r["timer_overhead_ns"] for r in runs]),
                "ops": {k: latency_summary([r["ops"][k] for r in runs]) for k in runs[0]["ops"]},
            },
        )

    def pipeline_latency():
        results = {}
        cases = [("unpaced_full_day", raw, 0.0)]
        cases += [(f"paced_{int(r)}_per_second", prefix, r) for r in args.rates]
        for label, source, rate in cases:
            command = [replay, "pipeline", str(source), str(rate), *cpus]
            runs = [run_json(command) for _ in range(args.runs)]
            results[label] = {
                "target_messages_per_second": rate,
                "messages": runs[0]["messages"],
                "messages_per_second": spread([r["messages_per_second"] for r in runs]),
                "latency_ns": latency_summary([r["latency_ns"] for r in runs]),
                "pinned": runs[0]["pinned"],
            }
        write(
            "pipeline_latency",
            {
                "benchmark": "threaded pipeline, parse timestamp to book-updated timestamp (ns)",
                "command": "bench_replay pipeline <file> <rate> " + " ".join(cpus),
                "paced_input": f"first {args.paced_messages} messages of the day",
                "results": results,
            },
        )

    def matching():
        runs = [run_json([replay, "match", str(args.match_orders)]) for _ in range(args.runs)]
        write(
            "matching_engine",
            {
                "benchmark": "matching engine on a synthetic order stream, single thread",
                "command": f"bench_replay match {args.match_orders}",
                "orders": args.match_orders,
                "trades_per_run": runs[0]["trades"],
                "orders_per_second": spread([r["orders_per_second"] for r in runs]),
                "latency_ns": latency_summary([r["latency_ns"] for r in runs]),
            },
        )

    def micro_benchmarks():
        micro_out = args.output / "micro_raw.json"
        subprocess.run(
            [
                micro,
                f"--benchmark_repetitions={args.runs}",
                "--benchmark_format=json",
                f"--benchmark_out={micro_out}",
                "--benchmark_out_format=json",
            ],
            check=True,
            capture_output=True,
            timeout=3600,
        )
        data = json.loads(micro_out.read_text(encoding="utf-8"))
        micro_out.unlink()
        repetitions = {}
        for b in data["benchmarks"]:
            if b.get("run_type") == "iteration":  # one row per repetition
                repetitions.setdefault(b["run_name"], []).append(b)
        rows = {
            name: {
                "time_unit": reps[0]["time_unit"],
                "real_time": spread([r["real_time"] for r in reps]),
                "items_per_second": spread([r["items_per_second"] for r in reps]),
            }
            for name, reps in repetitions.items()
        }
        context = {k: data["context"].get(k) for k in ("num_cpus", "mhz_per_cpu", "caches")}
        write(
            "micro",
            {
                "benchmark": "component micro-benchmarks (Google Benchmark)",
                "command": f"bench_micro --benchmark_repetitions={args.runs}",
                "context": context,
                "results": rows,
            },
        )

    benchmarks = {
        "parse_throughput": lambda: throughput("parse"),
        "book_throughput": lambda: throughput("book"),
        # The same parse on a prefix small enough to stay in the page cache: separates
        # parser cost from the cost of faulting in an 11 GB mapping.
        "parse_prefix_throughput": lambda: throughput(
            "parse", prefix, "parse_prefix_throughput", f"the first {args.paced_messages} messages"
        ),
        "book_op_latency": op_latency,
        "pipeline_latency": pipeline_latency,
        "matching_engine": matching,
        "micro": micro_benchmarks,
    }
    selected = args.only or list(benchmarks)
    unknown = set(selected) - set(benchmarks)
    if unknown:
        sys.exit(f"unknown benchmarks: {sorted(unknown)}")
    for name in selected:
        benchmarks[name]()


if __name__ == "__main__":
    main()
