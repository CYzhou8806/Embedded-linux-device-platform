#!/usr/bin/env python3
"""Regression test for analyze.py on sim_trace.py's synthetic traces:
every scenario must be attributed to its expected layer, and the two
controls (baseline, clock_jump) to none. Run: python3 test_analyze.py"""
import tempfile
import unittest
from pathlib import Path

import analyze
import sim_trace


class AttributesEveryScenario(unittest.TestCase):
    def test_all_scenarios(self):
        with tempfile.TemporaryDirectory() as tmp:
            for name in analyze.EXPECTED:
                d = Path(tmp) / name
                d.mkdir()
                (d / "trace.txt").write_text(sim_trace.simulate(name))
                r = analyze.analyze_scenario(d)
                with self.subTest(scenario=name):
                    self.assertEqual(r["origin"], analyze.EXPECTED[name], r)

    def test_parses_real_ftrace_lines(self):
        # Formats as printed by the 6.12 kernel, flags column included.
        lines = [
            "          <idle>-0       [002] d.h1.  4031.123456: custom_acq_irq: irq_ts_ns=4031123450000",
            " irq/183-spi0.0-412     [003] .....  4031.124010: custom_acq_drain: drained=1 drain_ns=520000 "
            "since_irq_ns=560000 err=0",
            "  device-service-530    [001] .....  4031.124100: tracing_mark_write: device-service: Running -> "
            "Recovering on Stall",
        ]
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "t.txt"
            p.write_text("\n".join(lines))
            ev = analyze.parse(p)
        self.assertEqual([e.name for e in ev], ["custom_acq_irq", "custom_acq_drain", "tracing_mark_write"])
        self.assertEqual(ev[1].f["drain_ns"], "520000")
        self.assertIn("Running -> Recovering", ev[2].body)


if __name__ == "__main__":
    unittest.main()
