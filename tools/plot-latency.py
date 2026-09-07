#!/usr/bin/env python3
"""Renders Plan.md V7's required charts from the scheduler-comparison
LatencyLogger CSVs in results/latency/: a bar chart of p99.9/max per
config (the tail-latency comparison the whole exercise is about), and a
log-scale latency-over-time scatter to show *where in the run* the tail
events happen, not just their magnitude.

The bar chart uses the 2026-09-07 repeated-measurement data
(results/latency/repeat-20260907/, 3x independent 20s runs per config)
rather than the original single-run CSVs - the single-run numbers were
shown to overturn on repeat for several configs (see docs/performance.md's
"Repeat" sections), so a chart built from them would visually contradict
the document's own corrected conclusions. Each bar is the median across
the 3 repeats; whiskers show the full min-max range, which is itself part
of the finding (some configs are far more variable than others).

Usage:
    python3 tools/plot-latency.py

Requires matplotlib (dev-machine only tool, not meant to run on the Pi).
Writes results/latency/comparison_bar.png and
results/latency/comparison_timeline.png.
"""
from __future__ import annotations

import csv
import pathlib
import statistics

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

LATENCY_DIR = pathlib.Path(__file__).resolve().parent.parent / "results" / "latency"
REPEAT_DIR = LATENCY_DIR / "repeat-20260907"

# (single-run csv stem, repeat-batch csv stem, display label) - order
# matters for the bar chart's story. The repeat stem differs from the
# original single-run stem for three configs (named differently when the
# repeat batch was collected).
RUNS = [
	("baseline_noload", "baseline_noload", "no load,\nno tuning"),
	("baseline_load", "baseline_load", "load,\nno tuning"),
	("irq_affinity_load", "irq_affinity", "load,\n+IRQ affinity"),
	("affinity_load", "cpu_affinity", "load,\n+CPU affinity"),
	("irq_and_cpu_affinity_load", "irq_and_cpu_separate", "load,\n+IRQ+CPU affinity\n(separate cores)"),
	("mlock_load", "mlock_load", "load,\n+mlockall"),
	("fifo_load", "fifo_load", "load,\n+SCHED_FIFO"),
	("all_load", "all_load", "load,\n+all three"),
]


def load_latencies_us(path: pathlib.Path) -> list[float]:
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


def repeat_stats(repeat_stem: str) -> tuple[float, float, float, float, float]:
	"""Returns (p999_median, p999_lo, p999_hi, max_median, max_lo, max_hi)
	across the 3 repeat runs for one config."""
	p999s, maxes = [], []
	for run in (1, 2, 3):
		values = load_latencies_us(REPEAT_DIR / f"{repeat_stem}_run{run}.csv")
		p999s.append(percentile(sorted(values), 99.9))
		maxes.append(max(values))
	return (
		statistics.median(p999s), min(p999s), max(p999s),
		statistics.median(maxes), min(maxes), max(maxes),
	)


def main() -> None:
	# --- Bar chart: median p99.9 and max per config, error bars = range across 3 repeats ---
	labels = [label for _, _, label in RUNS]
	stats = [repeat_stats(repeat_stem) for _, repeat_stem, _ in RUNS]
	p999_med = [s[0] for s in stats]
	p999_err = [[s[0] - s[1] for s in stats], [s[2] - s[0] for s in stats]]
	max_med = [s[3] for s in stats]
	max_err = [[s[3] - s[4] for s in stats], [s[5] - s[3] for s in stats]]

	x = range(len(RUNS))
	width = 0.35
	fig, ax = plt.subplots(figsize=(11, 5.5))
	ax.bar([i - width / 2 for i in x], p999_med, width, yerr=p999_err, capsize=3,
		   label="p99.9 (median of 3 runs)", color="#4C72B0")
	ax.bar([i + width / 2 for i in x], max_med, width, yerr=max_err, capsize=3,
		   label="max observed (median of 3 runs)", color="#C44E52")
	ax.set_ylabel("latency (us)")
	ax.set_title("V7: IRQ-to-userspace tail latency by configuration\n"
				 "(median of 3x independent 20s runs, real Pi 5 + MCU, first ~5s trimmed;\n"
				 "whiskers = full range across the 3 runs)")
	ax.set_xticks(list(x))
	ax.set_xticklabels(labels, fontsize=8)
	ax.legend()
	ax.grid(axis="y", alpha=0.3)
	fig.tight_layout()
	out1 = LATENCY_DIR / "comparison_bar.png"
	fig.savefig(out1, dpi=150)
	print(f"wrote {out1}")

	data = {stem: load_latencies_us(LATENCY_DIR / f"{stem}.csv") for stem, _, _ in RUNS}

	# --- Timeline scatter: where do the tail events happen? ---
	fig2, axes = plt.subplots(len(RUNS), 1, figsize=(10, 1.6 * len(RUNS)), sharex=False)
	for ax2, (stem, _, label) in zip(axes, RUNS):
		values = data[stem]
		ax2.scatter(range(len(values)), values, s=2, alpha=0.5, color="#4C72B0")
		ax2.set_yscale("log")
		ax2.set_ylabel(label.replace("\n", " "), fontsize=7)
		ax2.tick_params(axis="both", labelsize=7)
	axes[-1].set_xlabel("sample index (post-trim)")
	fig2.suptitle("V7: per-sample latency over time, log scale (tail events are sparse spikes)\n"
				  "(one representative 20s run per config - see comparison_bar.png for the repeated-measurement comparison)", fontsize=9)
	fig2.tight_layout()
	out2 = LATENCY_DIR / "comparison_timeline.png"
	fig2.savefig(out2, dpi=150)
	print(f"wrote {out2}")


if __name__ == "__main__":
	main()
