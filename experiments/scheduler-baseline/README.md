# scheduler-baseline

Background material for Plan.md V7, not part of the acquisition pipeline
itself. Before trusting a scheduler-related latency number measured
against the real MCU/driver/device-service chain, this is a smaller,
dependency-free way to first see what each scheduling knob actually does
on its own - a synthetic periodic wakeup loop instead of real hardware
I/O.

`cyclic.c` reimplements the core idea behind `cyclictest` (part of the
`rt-tests` suite): sleep until a fixed absolute deadline
(`clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, ...)`), measure how late
the wakeup actually was, repeat. See the comment block at the top of
`cyclic.c` for why `TIMER_ABSTIME` specifically (not a relative sleep) is
what makes each iteration's measurement independent of how late any
previous iteration ran.

## Build

```bash
make
```

## Run

```bash
./cyclic                          # SCHED_OTHER (default), no mlockall - baseline
./cyclic -m                       # + mlockall(MCL_CURRENT | MCL_FUTURE)
sudo ./cyclic -p 80 -m            # + SCHED_FIFO priority 80 (needs root/CAP_SYS_NICE)
./cyclic -i 200 -l 50000          # 200us interval (5kHz), 50000 iterations
```

Reports `min`/`avg`/`max` wakeup latency in microseconds. On an
unloaded system the difference between these configurations is usually
small; the point of this tool is to first establish that baseline, then
re-run the same configurations *while the system is under load*
(`stress-ng`) to see each knob's effect show up in the `max` column,
which is where scheduling problems actually show up - `avg` tends to look
fine even when a workload is missing its deadlines.

## Real results (real Pi 5, 2026-09-05)

Cross-compiled and run directly on this project's Pi 5 (no MCU/SPI
involved). Load = four `yes > /dev/null &` processes (one per core):

| Config | avg (us) | max (us) |
|---|---|---|
| No load, no tuning | 52.7 | 59.8 |
| Under load, no tuning | 54.5 | 3641.0 |
| Under load, + mlockall only | 58.1 | 5975.5 |
| Under load, + SCHED_FIFO 80 + mlockall | 2.4 | 10.7 |

Same shape as `docs/performance.md`'s real acquisition-pipeline results:
load alone blows up the max by ~60x, mlockall alone does essentially
nothing, and SCHED_FIFO not only recovers but beats the unloaded
baseline. Having two independent tools (this one, and the real hardware
measurement) agree is the point — it means the scheduling conclusion
isn't an artifact of `device-service`'s own code.

## What this is not

Not a replacement for measuring the real acquisition pipeline
(`docs/performance.md`'s IRQ-to-userspace latency, driven by
`userspace/device-service/`'s `latency_log_path` and the driver's
`irq_ts_ns` - see `driver/custom-acq/custom_acq.c`). This tool has no
SPI, no interrupts, no kernel driver involved at all - it exists purely
to build intuition about `SCHED_FIFO`/`mlockall`/CPU affinity in
isolation before layering them onto the real, harder-to-interpret
hardware measurement.
