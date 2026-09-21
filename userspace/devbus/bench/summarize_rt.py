#!/usr/bin/env python3
"""Summarize run_rt_matrix.sh results across kernels.

    python3 bench/summarize_rt.py results/devbus/pi5-yocto
    python3 bench/summarize_rt.py results/devbus/pi5-raspios

Expects one sub-directory per kernel release (as written by
run_rt_matrix.sh). Prints markdown tables and writes rt_matrix.png.
"""
import csv
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

INK, MUTED, GRID, AXIS, SURFACE = "#0b0b0b", "#898781", "#e1e0d9", "#c3c2b7", "#fcfcfb"
# validated categorical slots 1-3, fixed order: one per kernel
KERNEL_COLORS = ["#2a78d6", "#eb6834", "#1baf7a"]


# Two naming schemes, because the two platforms get their kernels from
# different places (see platforms/README.md):
#   Yocto card   self-built:  6.18.52-devbus-{std,rt}-iso+, plus stock 6.6.63
#   Pi OS card   vendor apt:  6.18.39+rpt-rpi-{2712,v8,v8-rt}
# In both cases the pair that differs *only* in CONFIG_PREEMPT_RT is the real
# comparison; the third kernel is "what the card ships with" and is labelled
# as such, since it differs in more than the preemption model (on the Yocto
# card a whole other version, on the Pi OS card 16K vs 4K pages).
def kernel_label(rel):
    ver = rel.split("-")[0].split("+")[0]
    if "devbus-rt" in rel:
        return f"{ver} PREEMPT_RT (self-built)"
    if "devbus-std" in rel:
        return f"{ver} PREEMPT (same source)"
    if rel.endswith("v8-rt"):
        return f"{ver} PREEMPT_RT (vendor)"
    if rel.endswith("-v8"):
        return f"{ver} PREEMPT, 4K (vendor)"
    if "2712" in rel:
        return f"{ver} stock, 16K pages"
    return f"{ver} stock Yocto"


def kernel_order(rel):
    """Shipped default first, then the non-RT/RT pair that differs only in
    CONFIG_PREEMPT_RT, so the RT bar always sits next to its own control."""
    if "2712" in rel or ("devbus" not in rel and "rpt-rpi" not in rel):
        return 0
    if rel.endswith("v8-rt") or "devbus-rt" in rel:
        return 2
    return 1


def cyclictest_stats(path):
    hist, overflow = {}, 0
    for line in open(path):
        if line.startswith("#"):
            if "Histogram Overflows" in line:
                overflow = int(line.split(":")[1].split()[0])
            continue
        parts = line.split()
        if len(parts) == 2 and parts[0].isdigit():
            n = int(parts[1])
            if n:
                hist[int(parts[0])] = n
    total = sum(hist.values()) + overflow
    if not total:
        return None

    def pct(q):
        target, acc = q * total, 0
        for us in sorted(hist):
            acc += hist[us]
            if acc >= target:
                return us
        return float("inf")

    mx = max(hist) if not overflow else float("inf")
    return {"p50": pct(0.5), "p99": pct(0.99), "p999": pct(0.999), "max": mx, "n": total}


def main(root):
    root = Path(root)
    # One directory per kernel release, identified by the file only
    # run_rt_matrix.sh writes - host.txt alone would also match sibling
    # directories such as e2e/ that hold a different kind of run.
    kernels = sorted([d for d in root.iterdir() if d.is_dir() and (d / "latency_matrix.csv").exists()],
                     key=lambda d: kernel_order(d.name))
    rows = {}
    print("### cyclictest (SCHED_FIFO 90, isolated core 3, 1 kHz), µs\n")
    print("| kernel | load | p50 | p99 | p99.9 | max |\n| --- | --- | --- | --- | --- | --- |")
    for k in kernels:
        for load in ("idle", "load"):
            s = cyclictest_stats(k / f"cyclictest_{load}.txt")
            if s:
                print(f"| {kernel_label(k.name)} | {load} | {s['p50']} | {s['p99']} | {s['p999']} | {s['max']} |")
    print("\n### devbus one-way latency, 64 B @ 1 kHz, µs\n")
    print("| kernel | config | p50 | p99 | p99.9 | max | sub CPU % |\n| --- | --- | --- | --- | --- | --- | --- |")
    for k in kernels:
        f = k / "latency_matrix.csv"
        if not f.exists():
            continue
        for r in csv.reader(open(f)):
            if not r or r[0].startswith("#") or len(r) < 13:
                continue
            cfg = r[0]
            p50, p99, p999, mx, cpu = (float(x) for x in (r[7], r[9], r[10], r[11], r[12]))
            rows.setdefault(cfg, {})[k.name] = (p50, p99, p999, mx)
            print(f"| {kernel_label(k.name)} | {cfg} | {p50:.1f} | {p99:.1f} | {p999:.1f} | {mx:.1f} | {cpu:.0f} |")

    # Repeats: the three numbers the kernel comparison hinges on, every run.
    print("\n### Repeated runs (all under load): max µs per run, first = matrix run\n")
    print("| kernel | cyclictest | devbus default | devbus tuned (futex) |\n| --- | --- | --- | --- |")
    for k in kernels:
        cyc = [cyclictest_stats(p) for p in [k / "cyclictest_load.txt"] + sorted(k.glob("cyclictest_load_r*.txt"))]
        cyc = [str(s["max"]) for s in cyc if s]
        dev = {"default-load": [], "tuned-futex-load": []}
        for cfg in dev:
            if k.name in rows.get(cfg, {}):
                dev[cfg].append(rows[cfg][k.name][3])
        rep = k / "repeats.csv"
        if rep.exists():
            for r in csv.reader(open(rep)):
                if len(r) >= 14 and r[1] in dev:
                    dev[r[1]].append(float(r[12]))
        fmt = lambda xs: " / ".join(f"{x:.0f}" for x in xs)
        print(f"| {kernel_label(k.name)} | {' / '.join(cyc)} | {fmt(dev['default-load'])} | {fmt(dev['tuned-futex-load'])} |")

    # Chart: worst-case (max) latency per config, one bar group per config,
    # one bar per kernel. Max is what RT is supposed to bound.
    cfgs = [c for c in ("default-idle", "default-load", "tuned-futex-load", "tuned-spin-load", "tuned-spin-load-nothrottle") if c in rows]
    fig, axes = plt.subplots(1, 2, figsize=(12, 4.8), facecolor=SURFACE, sharey=False)
    for ax, (idx, title) in zip(axes, [(1, "p99"), (3, "max (worst case)")]):
        ax.set_facecolor(SURFACE)
        width = 0.8 / max(1, len(kernels))
        for ki, k in enumerate(kernels):
            vals = [rows[c].get(k.name, (0, 0, 0, 0))[idx] for c in cfgs]
            xs = [i + (ki - (len(kernels) - 1) / 2) * width for i in range(len(cfgs))]
            ax.bar(xs, vals, width=width * 0.92, color=KERNEL_COLORS[ki % 3], label=kernel_label(k.name))
        ax.set_yscale("log")
        ax.set_xticks(range(len(cfgs)))
        short = {"default-idle": "default\nidle", "default-load": "default\n+load", "tuned-futex-load": "tuned futex\n+load",
                 "tuned-spin-load": "tuned spin\n+load", "tuned-spin-load-nothrottle": "tuned spin\n+load, no\nthrottling"}
        ax.set_xticklabels([short.get(c, c) for c in cfgs], fontsize=8, color=MUTED)
        ax.set_title(f"devbus 64 B @ 1 kHz: {title}", loc="left", fontsize=11, color=INK)
        ax.set_ylabel("µs (log scale)", color=MUTED)
        ax.grid(True, axis="y", color=GRID, linewidth=0.8)
        ax.set_axisbelow(True)
        ax.tick_params(colors=MUTED)
        for side in ("top", "right"):
            ax.spines[side].set_visible(False)
        for side in ("left", "bottom"):
            ax.spines[side].set_color(AXIS)
    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper right", ncol=3, frameon=False, fontsize=9, labelcolor=INK)
    platform = {"pi5-yocto": "Yocto Scarthgap", "pi5-raspios": "Raspberry Pi OS", "dev-host": "dev host"}.get(root.name, root.name)
    fig.suptitle(f"Raspberry Pi 5, {platform}: kernel preemption model x tuning", x=0.01, ha="left", fontsize=13, color=INK)
    fig.tight_layout(rect=(0, 0, 1, 0.90))
    out = root / "rt_matrix.png"
    fig.savefig(out, dpi=150, facecolor=SURFACE)
    print(f"\n{out}")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else ".")
