# M8: cross-layer diagnostics and fault injection

Plan.md V2/M8's acceptance criterion is that every injected fault can be
traced to the layer it happened in. The pieces:

| piece | where |
|---|---|
| Six tracepoints, one per layer boundary (`custom_acq_irq`, `_drain`, `_sample`, `_read`, `_spi_error`, `_fault`) | `driver/custom-acq/custom_acq_trace.h` |
| Four fault-injection knobs (`fault_drain_delay_us`, `fault_drop_every`, `fault_spi_error_every`, `fault_stall_ms`), all traced when they fire | `driver/custom-acq/custom_acq.c`, module parameters |
| Supervisor state transitions written to `trace_marker`, same clock | `userspace/device-service/src/main.cpp` |
| Scenario matrix, run on the board | `scenarios.sh` |
| Host wrapper: push, run, fetch, analyze | `run_on_pi.sh` |
| "Which layer went wrong first" | `analyze.py` |
| Synthetic traces, and a regression test for the analyzer | `sim_trace.py`, `test_analyze.py` |

## Scenarios

| scenario | how | expected first layer | expected supervisor reaction |
|---|---|---|---|
| baseline | nothing | none | none |
| drain_delay | +400 µs per drained sample | drain | none (still keeps up; latency rises) |
| drop | driver drops every 50th sample | driver | none; `gaps` counter rises |
| spi_error | every 200th register read fails | spi | none |
| stall | next drain pass sleeps 4.5 s (> `liveness_timeout_ms`) | drain | Running → Recovering → Running |
| slow_consumer | `SIGSTOP` device-service for 1 s | reader | none, but kfifo overflows (driver, spread) |
| overload | MCU at 3000 Hz, past the ~1680 Hz cliff | drain | none, or a stall if it collapses |
| clock_jump | wall clock +1 h | none | none (everything runs on `CLOCK_MONOTONIC`) |

## How the analyzer decides

For each layer it finds the first anomaly in the fault window, compared
with the second of clean trace before the inject marker. The earliest is
reported as the origin and the rest as where it spread to. The injected
cause is in the same trace, so every verdict is checked against ground
truth. Three cases that look like faults and aren't:

- **An IRQ gap covered by a long drain pass is the drain's fault.** The
  threaded IRQ is oneshot, so a stuck drain silences the GPIO line too.
- **A drain pass that never ends leaves no `custom_acq_drain` event.**
  The event is emitted when the pass returns, so this case (case-07's
  collapsed regime, or a stall longer than the capture) is detected as
  an edge with no pass end after it.
- **A `read()` gap while the kfifo is empty is not the reader's fault.**
  It only counts if samples were queued during the gap.

The first version got 4 of the 8 synthetic scenarios wrong, in exactly
those three ways, plus a slower drain that still keeps up. It now gets
all 8 (`python3 test_analyze.py`).

## Status

- **Done, built:** the tracepoints and knobs compile against both kernels
  this driver supports, 6.12 (A/B image) and 6.6 (main image). `modinfo`
  shows the parameters and the module carries the `custom_acq` trace
  events.
- **Done, tested:** the analyzer, on synthetic traces. That proves the
  attribution logic, not the hardware.
- **Done on the board (2026-10-02, image 1.3.2): 7/7 scenarios
  attributed to the expected layer** (`clock_jump` left out on the
  production card: its clock-save timer could persist the shifted time to
  `/data`). [Report](../../results/m8-fault-injection/20261002-132739/report.md).
  The first board run, on 1.3.1, found a real driver bug instead: one
  injected SPI error stopped acquisition for 3.9 s. That is
  [case 15](../../docs/debugging/case-15-one-spi-error-stops-acquisition.md),
  fixed in 1.3.2, where the same scenario shows 0 sequence gaps and no
  supervisor action. The same run showed the trace ring being overwritten
  (sched/irq events), so M8 now records `custom_acq` events only.

| scenario | first layer | sequence gaps | supervisor |
| --- | --- | --- | --- |
| baseline | none | 0 | — |
| drain_delay | drain | 16 | — |
| drop | driver | 60 (= 3 s × 1000/s ÷ 50, exact) | — |
| spi_error | spi | 0 | — |
| stall | drain | 1 (the soft reset restarts seq) | Running → Recovering → Running, first attempt |
| slow_consumer | reader | 1 | — |
| overload | drain | 16 | — |

  `run_on_pi.sh` uses `sudo -n` for the production image (admin, no root
  login); `SUDO= bash run_on_pi.sh` for a root-login card.

One limit on the A/B image: device-service runs as `acq` there and can't
write the root-only `trace_marker`, so supervisor transitions are not in
the trace. `analyze.py` falls back to the history in `device-ctl status`,
which is stamped with wall-clock time. Loosening tracefs permissions on
the production image would be the wrong trade.
