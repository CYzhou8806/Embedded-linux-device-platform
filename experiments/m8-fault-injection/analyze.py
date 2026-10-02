#!/usr/bin/env python3
"""Plan.md V2/M8: locate each injected fault in the layer it happened in.

Reads the per-scenario trace.txt files scenarios.sh collected and, for
each, compares the fault window against the clean lead-in before it. Every
layer gets the timestamp of its first anomaly; the earliest is where the
problem started, the rest are where it spread to. The injected cause is
in the same trace (custom_acq_fault events, and the scenario name), so
each verdict is checked against ground truth.

Layers, in pipeline order:
  mcu_irq   GPIO edges stop or thin out with no drain pass to explain it
  spi       a register operation failed (custom_acq_spi_error)
  drain     a threaded-handler pass holds the bus far longer than usual
  driver    a sample is lost between the drain and the kfifo
            (overflow, eviction, injected drop)
  reader    the kfifo holds data but userspace stops read()ing it

Two things that look like faults and aren't, and are excluded on purpose:
an IRQ gap that a long drain pass covers (the threaded IRQ is oneshot, so
a stuck drain silences the line too - that's the drain's fault), and a
read() gap while the kfifo is empty (nothing to read).

    python3 analyze.py RESULTS_DIR        -> markdown report on stdout
"""
from __future__ import annotations

import json
import re
import statistics
import sys
from dataclasses import dataclass, field
from pathlib import Path

LINE = re.compile(
    r"^\s*(?P<task>.+?)-(?P<pid>\d+)\s+\[(?P<cpu>\d+)\]\s+(?:\S+\s+)?"
    r"(?P<ts>\d+\.\d+):\s+(?P<event>\w+):\s+(?P<body>.*)$"
)
KV = re.compile(r"(\w+)=(\S+)")
KFIFO_SIZE = 128
LAYERS = ["mcu_irq", "spi", "drain", "driver", "reader"]

# What each scenario is expected to be attributed to. None = no layer may
# report an anomaly (baseline, and the clock jump negative control).
EXPECTED = {
    "baseline": None,
    "drain_delay": "drain",
    "drop": "driver",
    "spi_error": "spi",
    "stall": "drain",
    "slow_consumer": "reader",
    "overload": "drain",
    "clock_jump": None,
}


@dataclass
class Event:
    ts: float
    name: str
    f: dict
    body: str


@dataclass
class Window:
    irqs: list = field(default_factory=list)
    drains: list = field(default_factory=list)   # (start, end, drained, err)
    samples: list = field(default_factory=list)  # (ts, seq, age_ns, kfifo_len, outcome)
    reads: list = field(default_factory=list)    # (ts, samples, kfifo_left)
    spi_errors: list = field(default_factory=list)
    faults: list = field(default_factory=list)
    marks: list = field(default_factory=list)


def read_trace(d: Path) -> Path:
    """trace.txt, or trace.txt.xz as committed under results/."""
    return d / "trace.txt" if (d / "trace.txt").is_file() else d / "trace.txt.xz"


def parse(path: Path) -> list[Event]:
    events = []
    if path.suffix == ".xz":
        import lzma
        text = lzma.open(path, "rt", errors="replace").read()
    else:
        text = path.read_text(errors="replace")
    for line in text.splitlines():
        m = LINE.match(line)
        if not m:
            continue
        body = m["body"]
        events.append(Event(float(m["ts"]), m["event"], dict(KV.findall(body)), body))
    return events


def window(events: list[Event], t0: float, t1: float) -> Window:
    w = Window()
    for e in events:
        if not t0 <= e.ts < t1:
            continue
        f = e.f
        if e.name == "custom_acq_irq":
            w.irqs.append(e.ts)
        elif e.name == "custom_acq_drain":
            dur = int(f["drain_ns"]) / 1e9
            w.drains.append((e.ts - dur, e.ts, int(f["drained"]), int(f["err"])))
        elif e.name == "custom_acq_sample":
            w.samples.append((e.ts, int(f["seq"]), int(f["age_ns"]), int(f["kfifo_len"]), f["outcome"]))
        elif e.name == "custom_acq_read":
            w.reads.append((e.ts, int(f["samples"]), int(f["kfifo_left"])))
        elif e.name == "custom_acq_spi_error":
            w.spi_errors.append(e.ts)
        elif e.name == "custom_acq_fault":
            w.faults.append((e.ts, f.get("kind", "?")))
        elif e.name == "tracing_mark_write":
            w.marks.append((e.ts, e.body))
    return w


def gaps(times: list[float]) -> list[tuple[float, float]]:
    return [(a, b) for a, b in zip(times, times[1:])]


def p(values: list[float], q: float) -> float:
    if not values:
        return 0.0
    if len(values) == 1:
        return values[0]
    return statistics.quantiles(values, n=100, method="inclusive")[int(q) - 1]


def first_anomalies(base: Window, fault: Window, trace_end: float) -> dict[str, float]:
    """Timestamp of each layer's first anomaly in the fault window."""
    found: dict[str, float] = {}

    def note(layer: str, ts: float) -> None:
        if layer not in found or ts < found[layer]:
            found[layer] = ts

    # Gaps are measured across the inject marker too: the last edge or
    # read() before it is where a gap that spans it began.
    irqs = base.irqs[-1:] + fault.irqs
    reads = [r[0] for r in base.reads[-1:]] + [r[0] for r in fault.reads]

    # spi: any failed register operation (there are none on a healthy link).
    if fault.spi_errors:
        note("spi", fault.spi_errors[0])

    # drain, three ways it shows:
    #  1. one pass far longer than the baseline's worst; the anomaly starts
    #     when the pass started, not when it ended and got traced;
    base_drain = [e - s for s, e, _, _ in base.drains]
    limit = max(3 * p(base_drain, 99), 1.5 * max(base_drain, default=0.0), 0.005)
    for s, e, _, _ in fault.drains:
        if e - s > limit:
            note("drain", s)
            break
    #  2. a pass that never ended: an edge with no drain event after it by
    #     the end of the trace (case-07's collapsed regime, or a stall
    #     longer than the capture). The threaded IRQ is oneshot, so no
    #     further edges arrive either;
    last_drain_end = max((e for _, e, _, _ in base.drains + fault.drains), default=0.0)
    open_irqs = [t for t in fault.irqs if t > last_drain_end]
    if open_irqs and trace_end - open_irqs[0] > limit:
        note("drain", open_irqs[0])
    #  3. every pass still short, but each sample costs clearly more than it
    #     did: a slower drain that is still keeping up. Sustained over ten
    #     passes so one slow pass is not a fault.
    def cost(d):
        s, e, n, _ = d
        return (e - s) / n if n else None
    base_cost = [c for c in map(cost, base.drains) if c]
    if base_cost:
        cost_limit = 1.5 * p(base_cost, 99)
        run_start, run_len = None, 0
        for d in fault.drains:
            c = cost(d)
            if c is not None and c > cost_limit:
                run_start = d[0] if run_len == 0 else run_start
                run_len += 1
                if run_len >= 10:
                    note("drain", run_start)
                    break
            else:
                run_len = 0

    # driver: a sample that never reached userspace's side of the kfifo.
    for ts, _, _, _, outcome in fault.samples:
        if outcome in ("overflow", "evicted", "injected_drop"):
            note("driver", ts)
            break

    # mcu_irq: a gap between edges well beyond baseline that no drain pass
    # accounts for - neither a finished one nor one still open at the end.
    base_irq_gap = max((b - a for a, b in gaps(base.irqs)), default=0.0)
    irq_limit = max(5 * base_irq_gap, 0.05)
    for a, b in gaps(irqs):
        if b - a <= irq_limit:
            continue
        covered = sum(max(0.0, min(e, b) - max(s, a)) for s, e, _, _ in fault.drains)
        if covered >= 0.5 * (b - a):
            continue
        # A pass that *failed* right at the start of the gap is the cause:
        # the edge-triggered IRQ needs the FIFO drained to fire again, so a
        # drain that gave up on an SPI error silences the line. Seen on the
        # board (2026-10-02): the first version blamed the MCU for that.
        failed = [e for s, e, _, err in fault.drains if err != 0 and a - 0.001 <= s <= a + 0.05]
        if failed:
            note("spi" if fault.spi_errors else "drain", a)
        else:
            note("mcu_irq", a)
        break

    # reader: a gap between read()s while samples were sitting in the kfifo.
    base_read_gap = max((b - a for a, b in gaps([r[0] for r in base.reads])), default=0.0)
    read_limit = max(5 * base_read_gap, 0.05)
    for a, b in gaps(reads):
        if b - a <= read_limit:
            continue
        backlog = max((k for ts, _, _, k, o in fault.samples if a <= ts < b and o == "queued"), default=0)
        if backlog >= 8:
            note("reader", a)
            break
    return found


def supervisor_reaction(d: Path, fault: Window) -> list[str]:
    marks = [b.split("device-service: ", 1)[1] for _, b in fault.marks if "device-service: " in b]
    if marks:
        return marks
    # Unprivileged service (A/B image) can't write trace_marker: fall back
    # to the history in device-ctl status, which is wall-clock stamped.
    try:
        before = json.loads((d / "status-before.json").read_text())["history"]
        after = json.loads((d / "status-after.json").read_text())["history"]
    except (OSError, ValueError, KeyError):
        return []
    seen = {(h["at"], h["from"], h["to"], h["event"]) for h in before}
    return [f'{h["from"]} -> {h["to"]} on {h["event"]} ({h["reason"]})'
            for h in after if (h["at"], h["from"], h["to"], h["event"]) not in seen]


def counters(d: Path) -> dict:
    try:
        b = json.loads((d / "status-before.json").read_text())
        a = json.loads((d / "status-after.json").read_text())
        return {"gaps": a["gaps"] - b["gaps"], "faults": a["faults"] - b["faults"],
                "recoveries": a["recoveries"] - b["recoveries"]}
    except (OSError, ValueError, KeyError):
        return {}


def analyze_scenario(d: Path) -> dict:
    name = d.name
    events = parse(read_trace(d))
    inject = next((e.ts for e in events if e.name == "tracing_mark_write" and f"m8: inject {name}" in e.body), None)
    remove = next((e.ts for e in events if e.name == "tracing_mark_write" and f"m8: remove {name}" in e.body), None)
    if inject is None:
        return {"scenario": name, "error": "no 'm8: inject' marker in trace"}
    t_start = events[0].ts if events else inject
    t_end = events[-1].ts + 1 if events else inject
    base, fault = window(events, t_start, inject), window(events, inject, t_end)
    found = first_anomalies(base, fault, t_end - 1)
    order = sorted(found, key=found.get)
    origin = order[0] if order else None
    expected = EXPECTED.get(name, "?")
    return {
        "scenario": name,
        "expected": expected,
        "origin": origin,
        "correct": origin == expected if expected != "?" else None,
        "spread": [(l, round((found[l] - inject) * 1000, 1)) for l in order],
        "injected": sorted({k for _, k in fault.faults}),
        "fault_window_ms": round(((remove or t_end) - inject) * 1000),
        "supervisor": supervisor_reaction(d, fault),
        "counters": counters(d),
        "evidence": sorted(p.name for p in (d / "evidence").glob("fault-*.json")) if (d / "evidence").is_dir() else [],
    }


def report(results: list[dict]) -> str:
    out = ["# M8 fault-injection report", "",
           "| scenario | injected | expected layer | first anomaly | verdict | spread (ms after inject) | supervisor |",
           "|---|---|---|---|---|---|---|"]
    for r in results:
        if "error" in r:
            out.append(f'| {r["scenario"]} | | | | error: {r["error"]} | | |')
            continue
        verdict = {True: "correct", False: "**wrong**", None: "n/a"}[r["correct"]]
        spread = ", ".join(f"{l} {t:+.1f}" for l, t in r["spread"]) or "none"
        sup = "; ".join(r["supervisor"]) or "no transition"
        out.append(f'| {r["scenario"]} | {", ".join(r["injected"]) or "-"} | {r["expected"] or "none"} | '
                   f'{r["origin"] or "none"} | {verdict} | {spread} | {sup} |')
    ok = sum(1 for r in results if r.get("correct"))
    graded = sum(1 for r in results if r.get("correct") is not None)
    out += ["", f"{ok}/{graded} scenarios attributed to the expected layer."]
    return "\n".join(out)


def main() -> int:
    root = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    dirs = [d for d in sorted(root.iterdir()) if read_trace(d).is_file()]
    order = list(EXPECTED)
    dirs.sort(key=lambda d: order.index(d.name) if d.name in order else len(order))
    results = [analyze_scenario(d) for d in dirs]
    (root / "report.json").write_text(json.dumps(results, indent=2))
    print(report(results))
    return 0 if all(r.get("correct") is not False for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
