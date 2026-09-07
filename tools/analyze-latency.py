#!/usr/bin/env python3
"""Summarizes a device-service latency CSV (Plan.md V7) into the
percentiles Plan.md's report format asks for: median / p99 / p99.9 /
maximum observed (never "worst case" - see the note below).

The CSV comes from userspace/device-service's LatencyLogger
(config's `latency_log_path`), one row per sample: seq, irq_ts_ns (the
driver's ktime_get_ns() at the hard-IRQ that drained this sample),
recv_ts_ns (device-service's steady_clock receive time), latency_ns
(the difference, already computed by the logger).

Usage:
    python3 tools/analyze-latency.py results/latency/some-run.csv
    python3 tools/analyze-latency.py results/latency/some-run.csv --label "SCHED_FIFO+mlockall"

Stdlib only - deliberately no numpy/pandas dependency so this runs
directly on the Pi with nothing extra installed.
"""
from __future__ import annotations

import argparse
import csv
import statistics
import sys


def percentile(sorted_values: list[float], pct: float) -> float:
    """Nearest-rank percentile - simple and reproducible, good enough for
    a report note; no interpolation-method bikeshedding needed here."""
    if not sorted_values:
        return float("nan")
    idx = max(0, min(len(sorted_values) - 1, int(round(pct / 100.0 * len(sorted_values))) - 1))
    return sorted_values[idx]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("csv_path", help="LatencyLogger CSV output (seq,irq_ts_ns,recv_ts_ns,latency_ns)")
    parser.add_argument("--label", default=None, help="short name for this run, e.g. the scheduler config tested")
    args = parser.parse_args()

    latencies_us: list[float] = []
    with open(args.csv_path, newline="") as f:
        reader = csv.DictReader(f)
        if reader.fieldnames != ["seq", "irq_ts_ns", "recv_ts_ns", "latency_ns"]:
            print(f"warning: unexpected header {reader.fieldnames!r} - expected LatencyLogger's format",
                  file=sys.stderr)
        for row in reader:
            latencies_us.append(int(row["latency_ns"]) / 1000.0)

    if not latencies_us:
        print("no data rows found", file=sys.stderr)
        return 1

    latencies_us.sort()
    label = args.label or args.csv_path

    print(f"=== {label} ===")
    print(f"n_samples   = {len(latencies_us)}")
    print(f"min_us      = {latencies_us[0]:.1f}")
    print(f"median_us   = {statistics.median(latencies_us):.1f}")
    print(f"mean_us     = {statistics.fmean(latencies_us):.1f}")
    print(f"p99_us      = {percentile(latencies_us, 99):.1f}")
    print(f"p99.9_us    = {percentile(latencies_us, 99.9):.1f}")
    # "maximum observed", not "worst case" - a finite run can't measure a
    # theoretical upper bound, only report what actually happened
    # (Plan.md V7's report-format note).
    print(f"max_observed_us = {latencies_us[-1]:.1f}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
