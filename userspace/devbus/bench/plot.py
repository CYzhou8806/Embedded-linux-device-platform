#!/usr/bin/env python3
"""Plot latency vs. payload size from results/devbus/<platform>/latency.csv.

    python3 bench/plot.py results/devbus/<platform>

Writes latency_vs_size.png next to the CSV. Two small multiples (median
and p99) instead of one chart with two y-scales.
"""
import csv
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

# Validated categorical slots 1-3 (blue, orange, aqua), fixed order.
SERIES = [
    ("devbus-loan", "devbus loan (zero copy)", "#2a78d6"),
    ("devbus-copy", "devbus copy (1 memcpy)", "#eb6834"),
    ("uds", "Unix socket (2 kernel copies)", "#1baf7a"),
]
INK, MUTED, GRID, AXIS, SURFACE = "#0b0b0b", "#898781", "#e1e0d9", "#c3c2b7", "#fcfcfb"


def human(n):
    return f"{n // 1048576} MiB" if n >= 1048576 else f"{n // 1024} KiB" if n >= 1024 else f"{n} B"


def main(result_dir):
    result_dir = Path(result_dir)
    rows = [r for r in csv.reader(open(result_dir / "latency.csv")) if r and r[0] == "latency" and r[3] != "spin"]
    data = {}
    for r in rows:
        data.setdefault(r[1], []).append((int(r[2]), float(r[6]), float(r[8])))

    fig, axes = plt.subplots(1, 2, figsize=(11, 4.2), facecolor=SURFACE, sharex=True)
    for ax, (col, title) in zip(axes, [(1, "median one-way latency"), (2, "p99 one-way latency")]):
        ax.set_facecolor(SURFACE)
        for key, label, color in SERIES:
            pts = sorted(data.get(key, []))
            if not pts:
                continue
            xs = [p[0] for p in pts]
            ys = [p[col] for p in pts]
            ax.plot(xs, ys, color=color, linewidth=2, marker="o", markersize=6,
                    markeredgecolor=SURFACE, markeredgewidth=1.5, label=label)
            ax.annotate(key, (xs[-1], ys[-1]), xytext=(6, 0), textcoords="offset points",
                        va="center", fontsize=9, color=INK)
        ax.set_xscale("log", base=2)
        ax.set_yscale("log")
        ax.set_xticks([64, 4096, 65536, 1048576, 4194304])
        ax.set_xticklabels([human(x) for x in [64, 4096, 65536, 1048576, 4194304]])
        ax.set_title(title, loc="left", fontsize=11, color=INK)
        ax.set_xlabel("payload size", color=MUTED)
        ax.set_ylabel("µs (log scale)", color=MUTED)
        ax.grid(True, which="major", color=GRID, linewidth=0.8)
        ax.tick_params(colors=MUTED, which="both")
        for side in ("top", "right"):
            ax.spines[side].set_visible(False)
        for side in ("left", "bottom"):
            ax.spines[side].set_color(AXIS)
        ax.set_xlim(40, 4194304 * 3)
    axes[0].legend(frameon=False, fontsize=9, labelcolor=INK, loc="upper left")
    host = (result_dir / "host.txt").read_text().splitlines()[0].lstrip("# ")
    fig.suptitle("Same-machine IPC latency vs. payload size", x=0.01, ha="left", fontsize=13, color=INK)
    fig.text(0.01, 0.005, host, fontsize=8, color=MUTED)
    fig.tight_layout(rect=(0, 0.03, 1, 0.95))
    out = result_dir / "latency_vs_size.png"
    fig.savefig(out, dpi=150, facecolor=SURFACE)
    print(out)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else ".")
