#!/usr/bin/env python3
"""Batched-drain fraction, A (6.12, hardening.cfg only, image 1.2.x) vs
B (6.12 + hardening-kspp.cfg, image 1.3.1), same card, same MCU, 2026-10-02.

Each .bin is 12000 raw 16-byte samples read straight from /dev/acq0 with
device-service stopped (struct custom_acq_sample: u32 seq, u32 value,
s64 irq_ts_ns), MCU at 1000 Hz, inter_frame_us=50. The first 2000 are
dropped as warm-up. A repeated irq_ts_ns means two samples were drained
in one pass of the threaded handler (one IRQ edge, several samples) - the
quantity M5's calibration refuses above 1%.
"""
import glob
import os
import statistics
import struct

here = os.path.dirname(os.path.abspath(__file__))
rows = {}
for f in sorted(glob.glob(os.path.join(here, "*.bin"))):
    d = open(f, "rb").read()
    s = [struct.unpack_from("<IIq", d, i) for i in range(0, len(d) - 15, 16)][2000:]
    ts = [x[2] for x in s]
    seq = [x[0] for x in s]
    rep = 100 * sum(1 for a, b in zip(ts, ts[1:]) if a == b) / len(s)
    gaps = sum(1 for a, b in zip(seq, seq[1:]) if (b - a) & 0xFFFFFFFF != 1)
    name = os.path.basename(f)[:-4]
    rows.setdefault(name.rsplit("-", 1)[0], []).append(rep)
    print(f"{name:14s} samples {len(s)}  repeated irq_ts {rep:5.1f}%  seq gaps {gaps}")
for k, v in rows.items():
    print(f"{k:10s} mean {statistics.mean(v):5.1f}%  range {min(v):.1f}-{max(v):.1f}%")
