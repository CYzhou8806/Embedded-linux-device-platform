# Hardware stress test

`stress_test.py` — a standalone script (not pytest), for sustained-load
runs against real hardware. Requires MCU + Pi + `custom_acq.ko` already up
(`device-tree/README.md`), and nothing else reading `/dev/acq0`
concurrently (see `../integration/README.md` for why).

## Running

```bash
sudo python3 tests/hardware/stress_test.py --duration-seconds 30
```

`sudo` isn't strictly required (same permission model as
`tests/integration/` — reads are world-readable, `control` writes shell
out to `sudo tee` internally), but simplest to just run it as root.

Plan.md's V5 asks for a 10-minute run (`--duration-seconds 600`, the
default) reporting sequence gaps and error rate. A short run (tens of
seconds) is enough to sanity-check the script itself; the full 10-minute
run is a deliberate, separate action, not something to run routinely —
see the report format below and
`docs/debugging/case-06-spi-controller-stall-under-sustained-load.md` for
why a long run can end early with a detected stall instead of completing.

## Output

Prints a report to stdout and writes the same text to
`results/stress/<timestamp>.txt`: requested vs. actual duration, samples
read, sequence gaps, `kfifo_overflow` before/after, throughput, and
whether the run ended in a detected stall. Exit code is `1` on a stall,
`0` otherwise.

## Case 06 root-cause tracing (V7)

`case06_ftrace_spi.sh` runs `stress_test.py` under `ftrace`
(`function_graph`, filtered to every function with "spi" in its name) and
saves the trace plus the MCU's own `spi_rearm_fail`/`spi_error_count`
diagnostic counters (new sysfs attributes, V7) to
`results/ftrace/<timestamp>.txt`. Needs to run as root on the Pi itself
(uses `/sys/kernel/debug/tracing`):

```bash
sudo bash tests/hardware/case06_ftrace_spi.sh 30
```

It captures evidence, it doesn't diagnose - reading the resulting trace
to find where a stall actually blocks is manual analysis, see
`docs/debugging/case-06-spi-controller-stall-under-sustained-load.md`.
