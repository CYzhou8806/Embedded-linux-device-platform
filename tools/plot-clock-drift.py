#!/usr/bin/env python3
"""Renders Plan.md V2/M3's clock-drift chart from a long sustained
LatencyLogger capture at a known, fixed REG_SAMPLE_RATE
(results/clock-drift/1000hz_300s.csv - see docs/performance.md's "M3:
Clock Drift" section for the methodology and why this doesn't need any
MCU firmware change or new hardware: the MCU's own fixed-rate timer
already acts as a second clock to compare the Pi's against).

Two panels sharing the x-axis (sequence number, i.e. time):
- top: the naive (uncompensated) prediction error - assume the MCU
  produces exactly at the requested nominal rate, forever - grows
  linearly as the two crystals' real frequencies diverge.
- bottom: the same error after fitting the *actual* measured rate
  (ordinary least squares of irq_ts_ns vs seq) instead of the nominal
  one - bounded, no trend, just per-sample jitter.

Usage:
    python3 tools/plot-clock-drift.py [csv_path] [nominal_hz]

Requires matplotlib (dev-machine only tool, not meant to run on the Pi).
Writes results/clock-drift/drift_correction.png.
"""
from __future__ import annotations

import csv
import pathlib
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

RESULTS_DIR = pathlib.Path(__file__).resolve().parent.parent / "results" / "clock-drift"

TRIM_NS = 5_000_000_000  # skip the first 5s startup transient


def main() -> int:
    csv_path = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else RESULTS_DIR / "1000hz_300s.csv"
    nominal_hz = float(sys.argv[2]) if len(sys.argv) > 2 else 1000.0
    nominal_ns_per_sample = 1e9 / nominal_hz

    with open(csv_path, newline="") as f:
        rows = list(csv.DictReader(f))

    t0 = int(rows[0]["recv_ts_ns"])
    trimmed = [r for r in rows if int(r["recv_ts_ns"]) - t0 > TRIM_NS]
    seqs = [int(r["seq"]) for r in trimmed]
    irq = [int(r["irq_ts_ns"]) for r in trimmed]
    n = len(trimmed)

    mean_seq = sum(seqs) / n
    mean_irq = sum(irq) / n
    num = sum((s - mean_seq) * (t - mean_irq) for s, t in zip(seqs, irq))
    den = sum((s - mean_seq) ** 2 for s in seqs)
    slope = num / den
    intercept = mean_irq - slope * mean_seq
    drift_ppm = (slope - nominal_ns_per_sample) / nominal_ns_per_sample * 1e6

    s0, i0 = seqs[0], irq[0]
    naive_err_ms = [((i0 + (s - s0) * nominal_ns_per_sample) - t) / 1e6 for s, t in zip(seqs, irq)]
    comp_err_us = [((intercept + s * slope) - t) / 1e3 for s, t in zip(seqs, irq)]

    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(9, 7), sharex=True)

    ax1.plot(seqs, naive_err_ms, linewidth=1)
    ax1.set_ylabel("uncompensated error (ms)")
    ax1.set_title(f"Assuming exactly {nominal_hz:.0f}Hz forever - error grows linearly ({drift_ppm:+.1f} ppm drift)")
    ax1.grid(True, alpha=0.3)

    ax2.plot(seqs, comp_err_us, linewidth=0.5)
    ax2.set_xlabel("sequence number (= elapsed samples)")
    ax2.set_ylabel("compensated error (us)")
    ax2.set_title("Fitting the MCU's actual measured rate instead - bounded, no trend")
    ax2.grid(True, alpha=0.3)

    fig.tight_layout()
    out = RESULTS_DIR / "drift_correction.png"
    fig.savefig(out, dpi=150)
    print(f"drift: {drift_ppm:.2f} ppm, wrote {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
