#!/usr/bin/env python3
"""Synthetic traces for testing analyze.py without the board.

A tick-based model of the same pipeline the driver implements: an MCU
producing at a fixed rate into its FIFO, a rising-edge IRQ only when that
FIFO goes from empty to non-empty, a threaded drain that pays a fixed cost
per sample (three two-frame register reads) and keeps going until the FIFO
is empty, a 128-entry kfifo, and a reader that wakes when data arrives.
Each scenario perturbs the model the way the matching knob in
scenarios.sh perturbs the real one.

This tests the analyzer's logic, not the hardware: the costs are round
numbers, and nothing here says the real board behaves this way. The real
answer comes from scenarios.sh on the board.

    python3 sim_trace.py OUT_DIR    -> OUT_DIR/<scenario>/trace.txt
"""
from __future__ import annotations

import sys
from pathlib import Path

TICK = 50e-6
DRAIN_COST = 0.5e-3   # per sample, at inter_frame_us=50 roughly
READ_LATENCY = 100e-6
KFIFO = 128
MCU_FIFO = 32         # FIFO_DEPTH in the v1.3/v1.4 firmware's main.c
INJECT, FAULT_S, TAIL = 1.0, 3.0, 2.0


def simulate(scenario: str) -> str:
    lines: list[str] = []

    def emit(t: float, task: str, event: str, body: str) -> None:
        lines.append(f"{task:>16}-{100 if 'irq' in task else 200} [001] ..... {t + 1000:.6f}: {event}: {body}")

    rate = 1000.0
    t, end = 0.0, INJECT + FAULT_S + TAIL
    remove = INJECT + (1.0 if scenario == "slow_consumer" else FAULT_S)
    mcu_fifo: list[tuple[int, float]] = []
    next_sample, seq = 0.0, 0
    kfifo: list[int] = []
    draining, pass_start, busy_until, irq_ts = False, 0.0, 0.0, 0.0
    drained, stalled_once, reg_reads = 0, False, 0
    reader_due: float | None = None
    marked_inject = marked_remove = False

    while t < end:
        in_fault = INJECT <= t < remove
        if not marked_inject and t >= INJECT:
            emit(t, "sh", "tracing_mark_write", f"m8: inject {scenario}")
            marked_inject = True
        if not marked_remove and t >= remove:
            emit(t, "sh", "tracing_mark_write", f"m8: remove {scenario}")
            marked_remove = True

        r = 3000.0 if scenario == "overload" and in_fault else rate
        while next_sample <= t:
            was_empty = not mcu_fifo
            if len(mcu_fifo) < MCU_FIFO:  # full: the MCU loses it, a seq gap later
                mcu_fifo.append((seq, next_sample))
            seq += 1
            next_sample += 1.0 / r
            if was_empty and not draining:
                irq_ts = t
                emit(t, "<idle>-irq", "custom_acq_irq", f"irq_ts_ns={int(t * 1e9)}")
                draining, pass_start, busy_until, drained = True, t, t, 0
                if scenario == "stall" and in_fault and not stalled_once:
                    stalled_once = True
                    emit(t, "irq/custom_acq", "custom_acq_fault", "kind=stall arg=4500")
                    busy_until = t + 4.5

        if draining and t >= busy_until:
            if not mcu_fifo:
                emit(t, "irq/custom_acq", "custom_acq_drain",
                     f"drained={drained} drain_ns={int((t - pass_start) * 1e9)} "
                     f"since_irq_ns={int((t - irq_ts) * 1e9)} err=0")
                draining = False
            else:
                cost = DRAIN_COST
                reg_reads += 3
                if scenario == "spi_error" and in_fault and reg_reads % 200 < 3:
                    emit(t, "irq/custom_acq", "custom_acq_fault", "kind=spi_error arg=5")
                    emit(t, "irq/custom_acq", "custom_acq_spi_error", "cmd=0x05 echo=0x00 err=-5")
                    emit(t, "irq/custom_acq", "custom_acq_drain",
                         f"drained={drained} drain_ns={int((t - pass_start) * 1e9)} "
                         f"since_irq_ns={int((t - irq_ts) * 1e9)} err=-5")
                    draining = False
                    t += TICK
                    continue
                if scenario == "drain_delay" and in_fault:
                    emit(t, "irq/custom_acq", "custom_acq_fault", "kind=drain_delay arg=400")
                    cost += 400e-6
                s, born = mcu_fifo.pop(0)
                age = int((t - irq_ts) * 1e9)
                if scenario == "drop" and in_fault and s % 50 == 0:
                    emit(t, "irq/custom_acq", "custom_acq_fault", f"kind=drop arg={s}")
                    emit(t, "irq/custom_acq", "custom_acq_sample",
                         f"seq={s} age_ns={age} kfifo_len={len(kfifo)} outcome=injected_drop")
                elif len(kfifo) >= KFIFO:
                    emit(t, "irq/custom_acq", "custom_acq_sample",
                         f"seq={s} age_ns={age} kfifo_len={len(kfifo)} outcome=overflow")
                else:
                    kfifo.append(s)
                    emit(t, "irq/custom_acq", "custom_acq_sample",
                         f"seq={s} age_ns={age} kfifo_len={len(kfifo)} outcome=queued")
                    if reader_due is None:
                        reader_due = t + READ_LATENCY
                drained += 1
                busy_until = t + cost

        reader_stopped = scenario == "slow_consumer" and in_fault
        if reader_due is not None and t >= reader_due and not reader_stopped and kfifo:
            n = len(kfifo)
            kfifo.clear()
            emit(t, "device-service", "custom_acq_read", f"samples={n} kfifo_left=0")
            reader_due = None
        elif reader_stopped and reader_due is not None:
            reader_due = t  # will read as soon as it's resumed
        t += TICK
    return "\n".join(lines) + "\n"


def main() -> None:
    out = Path(sys.argv[1])
    for s in ["baseline", "drain_delay", "drop", "spi_error", "stall", "slow_consumer", "overload", "clock_jump"]:
        d = out / s
        d.mkdir(parents=True, exist_ok=True)
        (d / "trace.txt").write_text(simulate(s))


if __name__ == "__main__":
    main()
