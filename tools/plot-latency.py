#!/usr/bin/env python3
"""Renders Plan.md V7's required charts from the scheduler-comparison
LatencyLogger CSVs in results/latency/: a bar chart of p99.9/max per
config (the tail-latency comparison the whole exercise is about), and a
log-scale latency-over-time scatter to show *where in the run* the tail
events happen, not just their magnitude.

Usage:
    python3 tools/plot-latency.py

Requires matplotlib (dev-machine only tool, not meant to run on the Pi).
Writes results/latency/comparison_bar.png and
results/latency/comparison_timeline.png.
"""
from __future__ import annotations

import csv
import pathlib

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

LATENCY_DIR = pathlib.Path(__file__).resolve().parent.parent / "results" / "latency"

# (csv stem, display label) - order matters for the bar chart's story.
RUNS = [
	("baseline_noload", "no load,\nno tuning"),
	("baseline_load", "load,\nno tuning"),
	("irq_affinity_load", "load,\n+IRQ affinity"),
	("affinity_load", "load,\n+CPU affinity"),
	("irq_and_cpu_affinity_load", "load,\n+IRQ+CPU affinity\n(separate cores)"),
	("mlock_load", "load,\n+mlockall"),
	("fifo_load", "load,\n+SCHED_FIFO"),
	("all_load", "load,\n+all three"),
]


def load_latencies_us(stem: str) -> list[float]:
	path = LATENCY_DIR / f"{stem}.csv"
	values = []
	with open(path, newline="") as f:
		reader = csv.DictReader(f)
		for i, row in enumerate(reader):
			if i < 5000:  # skip startup transient, matches docs/performance.md's methodology
				continue
			values.append(int(row["latency_ns"]) / 1000.0)
	return values


def percentile(sorted_values: list[float], pct: float) -> float:
	idx = max(0, min(len(sorted_values) - 1, int(round(pct / 100.0 * len(sorted_values))) - 1))
	return sorted_values[idx]


def main() -> None:
	data = {stem: load_latencies_us(stem) for stem, _ in RUNS}

	# --- Bar chart: p99.9 and max per config ---
	labels = [label for _, label in RUNS]
	p999s = [percentile(sorted(data[stem]), 99.9) for stem, _ in RUNS]
	maxes = [max(data[stem]) for stem, _ in RUNS]

	x = range(len(RUNS))
	width = 0.35
	fig, ax = plt.subplots(figsize=(11, 5.5))
	ax.bar([i - width / 2 for i in x], p999s, width, label="p99.9", color="#4C72B0")
	ax.bar([i + width / 2 for i in x], maxes, width, label="max observed", color="#C44E52")
	ax.set_ylabel("latency (us)")
	ax.set_title("V7: IRQ-to-userspace tail latency by configuration\n(20s runs, real Pi 5 + MCU, first ~5s trimmed)")
	ax.set_xticks(list(x))
	ax.set_xticklabels(labels, fontsize=8)
	ax.legend()
	ax.grid(axis="y", alpha=0.3)
	fig.tight_layout()
	out1 = LATENCY_DIR / "comparison_bar.png"
	fig.savefig(out1, dpi=150)
	print(f"wrote {out1}")

	# --- Timeline scatter: where do the tail events happen? ---
	fig2, axes = plt.subplots(len(RUNS), 1, figsize=(10, 1.6 * len(RUNS)), sharex=False)
	for ax2, (stem, label) in zip(axes, RUNS):
		values = data[stem]
		ax2.scatter(range(len(values)), values, s=2, alpha=0.5, color="#4C72B0")
		ax2.set_yscale("log")
		ax2.set_ylabel(label.replace("\n", " "), fontsize=7)
		ax2.tick_params(axis="both", labelsize=7)
	axes[-1].set_xlabel("sample index (post-trim)")
	fig2.suptitle("V7: per-sample latency over time, log scale (tail events are sparse spikes)", fontsize=10)
	fig2.tight_layout()
	out2 = LATENCY_DIR / "comparison_timeline.png"
	fig2.savefig(out2, dpi=150)
	print(f"wrote {out2}")


if __name__ == "__main__":
	main()
