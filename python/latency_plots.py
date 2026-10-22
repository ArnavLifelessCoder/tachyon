"""Latency CDF plots from the histogram buckets in results/m6.

Each benchmark result keeps the median run's histogram buckets as
[upper_bound_ns, count] pairs. A CDF on a log x-axis shows the body and the tail
of each distribution together.

  python python/latency_plots.py --results results/m6
"""

import argparse
import json
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402


def cdf(buckets):
    total = sum(count for _, count in buckets)
    xs, ys, seen = [], [], 0
    for upper, count in buckets:
        seen += count
        xs.append(max(upper, 1))
        ys.append(seen / total)
    return xs, ys


def plot(series, title, path):
    fig, ax = plt.subplots(figsize=(7, 4.2))
    for label, buckets in series:
        xs, ys = cdf(buckets)
        ax.step(xs, ys, where="post", label=label)
    ax.set_xscale("log")
    ax.set_xlabel("latency (ns, log scale)")
    ax.set_ylabel("fraction of samples at or below")
    ax.set_ylim(0, 1.01)
    ax.grid(True, which="both", alpha=0.3)
    ax.set_title(title)
    ax.legend(loc="lower right", fontsize=8)
    fig.tight_layout()
    fig.savefig(path, dpi=130)
    plt.close(fig)
    print(f"wrote {path}")


NAMES = {
    "A": "add (A)",
    "F": "add with MPID (F)",
    "E": "execute (E)",
    "C": "execute with price (C)",
    "X": "cancel (X)",
    "D": "delete (D)",
    "U": "replace (U)",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--results", type=Path, default=Path("results/m6"))
    args = parser.parse_args()
    r = args.results

    ops = json.loads((r / "book_op_latency.json").read_text())
    plot(
        [(NAMES[k], v["median_run_buckets"]) for k, v in ops["ops"].items()],
        "Book update latency per ITCH message, full day",
        r / "book_op_latency_cdf.png",
    )
    pipeline = json.loads((r / "pipeline_latency.json").read_text())
    plot(
        [
            (label.replace("_", " "), v["latency_ns"]["median_run_buckets"])
            for label, v in pipeline["results"].items()
        ],
        "Pipeline latency, parse to book updated",
        r / "pipeline_latency_cdf.png",
    )
    engine = json.loads((r / "matching_engine.json").read_text())
    plot(
        [("submit (new, cancel, modify)", engine["latency_ns"]["median_run_buckets"])],
        "Matching engine latency per request",
        r / "matching_latency_cdf.png",
    )


if __name__ == "__main__":
    main()
