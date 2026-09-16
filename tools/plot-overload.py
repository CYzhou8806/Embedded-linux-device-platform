#!/usr/bin/env python3
"""Renders Plan.md V2/M0's overload sweep chart from
results/overload/rate_<target_hz>.csv (LatencyLogger CSVs captured at
increasing REG_SAMPLE_RATE targets, see docs/performance.md's "M0:
Overload Behavior" section for the methodology).

Two panels sharing the x-axis (target MCU sample rate, Hz):
- top: delivered rate (samples/sec, from timestamps) and sequence-loss
  percentage (from seq gaps) - the throughput/loss half of the "rate,
  latency, loss" curve M0 asks for.
- bottom: median/p99/p99.9 latency (us, log scale) for the runs where
  it's still a meaningful number (loss ~0%) - once loss jumps, the
  existing latency metric degenerates the same way case-07 found on the
  RT-comparison card (one huge continuous drain batch, stale irq_ts_ns),
  so those points are intentionally omitted rather than plotted as
  million-microsecond noise.

Usage:
    python3 tools/plot-overload.py

Requires matplotlib (dev-machine only tool, not meant to run on the Pi).
Writes results/overload/overload_curve.png.
"""
from __future__ import annotations

import csv
import pathlib
import re
import statistics

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

RESULTS_DIR = pathlib.Path(__file__).resolve().parent.parent / "results" / "overload"

# Trim the first 2s of each run (startup transient) before computing
# stats - same rationale as docs/performance.md's "trim the first ~5s",
# scaled down because these runs are much shorter (8-15s, not 20s).
TRIM_NS = 2_000_000_000


def percentile(sorted_values: list[float], pct: float) -> float:
    idx = max(0, min(len(sorted_values) - 1, int(round(pct / 100.0 * len(sorted_values))) - 1))
    return sorted_values[idx]


def load(path: pathlib.Path) -> dict:
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    if not rows:
        return {"n": 0}

    seqs = [int(r["seq"]) for r in rows]
    ts0 = int(rows[0]["recv_ts_ns"])
    ts_last = int(rows[-1]["recv_ts_ns"])
    dur_s = (ts_last - ts0) / 1e9

    lost = 0
    for a, b in zip(seqs, seqs[1:]):
        d = b - a
        if d > 1:
            lost += d - 1
    produced = len(rows) + lost
    loss_pct = 100.0 * lost / produced if produced else 0.0
    delivered_rate = len(rows) / dur_s if dur_s > 0 else 0.0

    trimmed = [r for r in rows if int(r["recv_ts_ns"]) - ts0 > TRIM_NS]
    lat_us = sorted(int(r["latency_ns"]) / 1000.0 for r in trimmed) if trimmed else []

    return {
        "n": len(rows),
        "dur_s": dur_s,
        "delivered_rate": delivered_rate,
        "loss_pct": loss_pct,
        "lat_us": lat_us,
    }


def main() -> int:
    files = sorted(RESULTS_DIR.glob("rate_*.csv"),
                    key=lambda p: int(re.match(r"rate_(\d+)\.csv", p.name).group(1)))

    targets, delivered, loss, medians, p99s, p999s = [], [], [], [], [], []
    for f in files:
        target = int(re.match(r"rate_(\d+)\.csv", f.name).group(1))
        d = load(f)
        # total-stall runs: either almost no rows, or all rows arrived in
        # one instant initial burst before the link went silent (e.g.
        # rate_7000.csv: 127 rows in <1ms, then nothing for the rest of
        # the run) - dur_s stays near zero, which would otherwise divide
        # out into a nonsense multi-hundred-thousand samples/s point.
        if d["n"] < 10 or d.get("dur_s", 0) < 1.0:
            continue
        targets.append(target)
        delivered.append(d["delivered_rate"])
        loss.append(d["loss_pct"])
        if d["loss_pct"] < 1.0 and d["lat_us"]:
            medians.append((target, statistics.median(d["lat_us"])))
            p99s.append((target, percentile(d["lat_us"], 99)))
            p999s.append((target, percentile(d["lat_us"], 99.9)))

    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(9, 8), sharex=True)

    ax1.plot(targets, delivered, "o-", color="tab:blue", label="delivered rate (samples/s)")
    ax1.set_ylabel("delivered rate (samples/s)", color="tab:blue")
    ax1.tick_params(axis="y", labelcolor="tab:blue")
    ax1b = ax1.twinx()
    ax1b.plot(targets, loss, "s--", color="tab:red", label="sequence loss (%)")
    ax1b.set_ylabel("sequence loss (%)", color="tab:red")
    ax1b.tick_params(axis="y", labelcolor="tab:red")
    ax1.set_title("M0 overload sweep: throughput and loss vs. requested MCU sample rate")
    ax1.grid(True, alpha=0.3)

    if medians:
        ax2.plot(*zip(*medians), "o-", label="median")
        ax2.plot(*zip(*p99s), "^-", label="p99")
        ax2.plot(*zip(*p999s), "s-", label="p99.9")
    ax2.set_yscale("log")
    ax2.set_xlabel("requested MCU sample rate (Hz, REG_SAMPLE_RATE)")
    ax2.set_ylabel("IRQ-to-userspace latency (us, log scale)")
    ax2.set_title("Latency in the clean (loss < 1%) regime only - see docs/performance.md")
    ax2.legend()
    ax2.grid(True, alpha=0.3)

    fig.tight_layout()
    out = RESULTS_DIR / "overload_curve.png"
    fig.savefig(out, dpi=150)
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
