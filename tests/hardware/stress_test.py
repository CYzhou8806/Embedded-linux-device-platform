#!/usr/bin/env python3
"""Sustained-load stress test against real hardware. Not a pytest test -
run standalone (see README below). Reports throughput, sequence gaps, and
kfifo_overflow growth like Plan.md's V5 asks; it's also deliberately built
to *detect and report* the case-06 SPI-controller stall
(docs/debugging/case-06-spi-controller-stall-under-sustained-load.md)
rather than hang forever if it hits it - that stall is itself one of the
things this stress test exists to surface.

Usage:
    sudo python3 tests/hardware/stress_test.py --duration-seconds 30

Requires MCU + Pi + custom_acq.ko already up (device-tree/README.md), and
nothing else reading /dev/acq0 concurrently (see tests/integration/README.md
for why two consumers split the sample stream and corrupt gap counting).
"""
from __future__ import annotations

import argparse
import datetime
import pathlib
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "integration"))
from acq_device import AcqDevice, AcqStall  # noqa: E402

STALL_TIMEOUT_S = 8.0  # generous - see case-06's bursty-production note
RESULTS_DIR = pathlib.Path(__file__).resolve().parent.parent.parent / "results" / "stress"


def run(duration_s: float) -> dict:
    acq = AcqDevice()
    acq.open()

    result = {
        "duration_s": duration_s,
        "samples_read": 0,
        "gaps": 0,
        "kfifo_overflow_before": None,
        "kfifo_overflow_after": None,
        "stalled": False,
        "stall_after_samples": None,
        "elapsed_s": None,
    }

    try:
        result["kfifo_overflow_before"] = acq.kfifo_overflow()
        acq.start()

        last_seq = None
        t0 = time.monotonic()
        while time.monotonic() - t0 < duration_s:
            try:
                seq, _value = acq.read_sample(timeout_s=STALL_TIMEOUT_S)
            except AcqStall:
                result["stalled"] = True
                result["stall_after_samples"] = result["samples_read"]
                break
            if last_seq is not None:
                expected = (last_seq + 1) & 0xFFFFFFFF
                if seq != expected:
                    result["gaps"] += 1
            last_seq = seq
            result["samples_read"] += 1
        result["elapsed_s"] = time.monotonic() - t0
    finally:
        if not result["stalled"]:
            try:
                result["kfifo_overflow_after"] = acq.kfifo_overflow()
            except Exception:
                pass
            try:
                acq.stop()
            except Exception as e:
                print(f"warning: stop() failed during cleanup: {e}", file=sys.stderr)
        acq.close()

    return result


def format_report(result: dict) -> str:
    lines = [
        f"stress_test run at {datetime.datetime.now().isoformat(timespec='seconds')}",
        f"requested duration: {result['duration_s']}s, actual: {result['elapsed_s']:.1f}s",
        f"samples read: {result['samples_read']}",
        f"sequence gaps: {result['gaps']}",
    ]
    if result["kfifo_overflow_before"] is not None:
        lines.append(f"kfifo_overflow: {result['kfifo_overflow_before']} -> "
                      f"{result['kfifo_overflow_after']}")
    if result["samples_read"] and result["elapsed_s"]:
        lines.append(f"throughput: {result['samples_read'] / result['elapsed_s']:.1f} samples/s")
    if result["stalled"]:
        lines.append(
            f"STALLED after {result['stall_after_samples']} samples - no data for "
            f"{STALL_TIMEOUT_S}s. See "
            "docs/debugging/case-06-spi-controller-stall-under-sustained-load.md. "
            "A physical MCU reset is likely needed before running anything else."
        )
    else:
        lines.append("completed without a stall.")
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--duration-seconds", type=float, default=600.0,
                         help="target run length (Plan.md's V5 asks for 10 minutes; "
                              "default 600s, use a smaller value for a quick check)")
    args = parser.parse_args()

    result = run(args.duration_seconds)
    report = format_report(result)
    print(report)

    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    out_path = RESULTS_DIR / f"{datetime.datetime.now():%Y%m%d-%H%M%S}.txt"
    out_path.write_text(report)
    print(f"report written to {out_path}")

    return 1 if result["stalled"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
