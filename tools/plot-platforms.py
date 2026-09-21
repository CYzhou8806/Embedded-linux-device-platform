#!/usr/bin/env python3
"""Two charts for the cross-platform work, regenerated from committed CSVs.

    python3 tools/plot-platforms.py

Writes results/devbus/leading-indicator.png and
results/devbus/platform-comparison.png. Both read the same raw data the
docs quote, so a number can never drift from its picture.
"""
import csv
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
RESULTS = ROOT / "results" / "devbus"

# Same tokens as userspace/devbus/bench/summarize_rt.py, so every chart in
# this repo reads as one set. Categorical slots 1-2 of the validated
# palette: worst adjacent CVD dE 24.7, normal-vision 33.6, both pass.
INK, MUTED, GRID, AXIS, SURFACE = "#0b0b0b", "#898781", "#e1e0d9", "#c3c2b7", "#fcfcfb"
SERIES_1, SERIES_2 = "#2a78d6", "#eb6834"
CRITICAL = "#d03b3b"


def style(ax):
    ax.set_facecolor(SURFACE)
    ax.grid(True, axis="y", color=GRID, linewidth=0.8)
    ax.set_axisbelow(True)
    ax.tick_params(colors=MUTED, labelsize=9)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(AXIS)


def read_ifus_sweep(path):
    rows = []
    for r in csv.DictReader(l for l in open(path) if not l.startswith("#")):
        # A collapsed point has no valid latency and no rx_per_s - only a
        # throughput figure - and it is the most important row in the file.
        if r["p50_us"] or r["rx_per_s"] or r["throughput_per_s"]:
            rows.append(r)
    return rows


def leading_indicator():
    """Throughput, gaps and overflow all read healthy while median latency
    climbs 11x - then one step later the pipeline collapses."""
    rows = read_ifus_sweep(RESULTS / "pi5-raspios" / "ifus-sweep" / "summary.csv")
    clean = [r for r in rows if r["p50_us"]]
    x = [int(r["inter_frame_us"]) for r in clean]
    lat = [float(r["p50_us"]) / 1000.0 for r in clean]  # ms
    # Only the part of the sweep the chart is about: 20-200 us was measured,
    # but past 100 the pipeline is already in the degraded regime and the
    # extra points just compress the interesting range.
    rate_x, rate_y = [], []
    for r in rows:
        v = r["rx_per_s"] or r["throughput_per_s"]
        if v and int(r["inter_frame_us"]) <= 100:
            rate_x.append(int(r["inter_frame_us"]))
            rate_y.append(float(v))
    cliff = 95  # between the last clean point (90) and the collapse (100)

    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(9, 6.4), sharex=True,
                                   facecolor=SURFACE, gridspec_kw={"height_ratios": [1.25, 1]})
    for ax in (ax1, ax2):
        style(ax)
        ax.axvspan(cliff, 106, color=CRITICAL, alpha=0.07, lw=0)
        ax.axvline(cliff, color=CRITICAL, lw=1.4, ls=(0, (4, 3)))
        ax.set_xlim(25, 106)

    ax1.plot(x, lat, color=SERIES_1, lw=2, marker="o", ms=8, zorder=3,
             markeredgecolor=SURFACE, markeredgewidth=2)
    ax1.set_ylabel("median latency (ms)", color=MUTED, fontsize=10)
    ax1.set_title("What the pipeline was actually doing", loc="left", fontsize=12, color=INK)
    for xi, yi in ((x[0], lat[0]), (x[-1], lat[-1])):
        ax1.annotate(f"{yi:.2f} ms", (xi, yi), textcoords="offset points", xytext=(0, 12),
                     ha="center", fontsize=9, color=INK)
    ax1.set_ylim(0, max(lat) * 1.25)

    ax2.plot(rate_x, rate_y, color=SERIES_2, lw=2, marker="o", ms=8, zorder=3,
             markeredgecolor=SURFACE, markeredgewidth=2)
    ax2.set_ylabel("delivered samples/s", color=MUTED, fontsize=10)
    ax2.set_xlabel("driver inter_frame_us setting (µs)", color=MUTED, fontsize=10)
    ax2.set_title("What every metric an operator watches was saying", loc="left",
                  fontsize=12, color=INK)
    ax2.set_ylim(0, 1250)
    ax2.annotate("no sequence gaps, no kfifo overflow anywhere left of the line",
                 (41, 1130), fontsize=9, color=MUTED)
    ax2.annotate(f"{rate_y[0]:.0f}/s", (rate_x[0], rate_y[0]), textcoords="offset points",
                 xytext=(0, -20), ha="center", fontsize=9, color=INK)
    ax2.annotate(f"{rate_y[-1]:.0f}/s", (rate_x[-1], rate_y[-1]), textcoords="offset points",
                 xytext=(0, -22), ha="center", fontsize=10, color=CRITICAL)

    ax1.annotate("no longer measurable:\nevery sample now carries\none stale timestamp",
                 (96.5, max(lat) * 0.62), fontsize=8.5, color=CRITICAL)
    fig.suptitle("Latency leads, throughput and loss lag — Raspberry Pi 5, 1 kHz acquisition",
                 x=0.012, ha="left", fontsize=13.5, color=INK)
    fig.text(0.012, 0.945,
             "Median latency climbed 11× while delivered rate, sequence gaps and the driver's\n"
             "overflow counter all stayed healthy — then one step later the pipeline collapsed.",
             fontsize=9.5, color=MUTED, ha="left", va="top")
    fig.tight_layout(rect=(0, 0, 1, 0.885))
    out = RESULTS / "leading-indicator.png"
    fig.savefig(out, dpi=150, facecolor=SURFACE)
    print(out)


def p999_default_load(kernel_dir):
    f = kernel_dir / "latency_matrix.csv"
    for r in csv.reader(open(f)):
        if r and r[0] == "default-load" and len(r) > 10:
            return float(r[10])
    return None


def platform_comparison():
    """The same workload on two distributions. The preemption model moves the
    number; which distribution it runs on does not."""
    yocto, raspios = RESULTS / "pi5-yocto", RESULTS / "pi5-raspios"
    groups = [
        ("6.6 PREEMPT\n(stock BSP kernel)",
         p999_default_load(yocto / "6.6.63-v8-16k"), None),
        ("6.18 PREEMPT",
         p999_default_load(yocto / "6.18.52-devbus-std-iso+"),
         p999_default_load(raspios / "6.18.39+rpt-rpi-v8")),
        ("6.18 PREEMPT_RT",
         p999_default_load(yocto / "6.18.52-devbus-rt-iso+"),
         p999_default_load(raspios / "6.18.39+rpt-rpi-v8-rt")),
    ]
    fig, ax = plt.subplots(figsize=(9.5, 4.8), facecolor=SURFACE)
    style(ax)
    ax.grid(True, axis="x", color=GRID, linewidth=0.8)
    ax.grid(False, axis="y")
    h = 0.34
    for i, (label, a, b) in enumerate(groups):
        for j, (val, color, name) in enumerate(((a, SERIES_1, "Yocto Scarthgap (kernels built here)"),
                                                (b, SERIES_2, "Raspberry Pi OS (vendor kernels)"))):
            if val is None:
                continue
            y = i + (j - 0.5) * (h + 0.03)
            ax.barh(y, val, height=h, color=color, zorder=3,
                    label=name if i == 1 else None)
            ax.annotate(f"{val:,.0f} µs".replace(",", " "), (val, y), textcoords="offset points",
                        xytext=(7, 0), va="center", fontsize=9.5, color=INK)
    ax.set_xscale("log")
    ax.set_xlim(20, 30000)
    ax.set_yticks(range(len(groups)))
    ax.set_yticklabels([g[0] for g in groups], fontsize=10, color=INK)
    ax.set_xlabel("p99.9 one-way latency under load, untuned consumer (µs, log scale)",
                  color=MUTED, fontsize=10)
    ax.invert_yaxis()
    ax.legend(loc="lower right", frameon=False, fontsize=9.5, labelcolor=INK)
    fig.suptitle("Same board, same workload: the kernel moves the number, the distribution does not",
                 x=0.012, ha="left", fontsize=13.5, color=INK)
    fig.text(0.012, 0.945,
             "A minimal Yocto image and a full Debian install land within 0.7% of each other.\n"
             "PREEMPT_RT is ~100× away from both, and replicates on kernels built by neither side.",
             fontsize=9.5, color=MUTED, ha="left", va="top")
    fig.tight_layout(rect=(0, 0, 1, 0.845))
    out = RESULTS / "platform-comparison.png"
    fig.savefig(out, dpi=150, facecolor=SURFACE)
    print(out)


if __name__ == "__main__":
    leading_indicator()
    platform_comparison()
