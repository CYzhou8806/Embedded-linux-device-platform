# Case 15: One Transient SPI Error Stopped Acquisition for Four Seconds

**Platform:** Raspberry Pi 5, image 1.3.1 → 1.3.2, custom-acq driver with the M8 tracepoints and fault-injection knobs, MCU firmware v1.4 at 1000 Hz
**Occurred:** 2026-10-02, the first on-board run of the M8 fault-injection matrix ([experiments/m8-fault-injection](../../experiments/m8-fault-injection/README.md))

M8's purpose is to show that a fault can be located in its layer. Its
first run on the board did that, and also turned up a real driver bug.

## Observation

Scenario `spi_error`: `fault_spi_error_every=200`, so one register read
in 200 returns -EIO. With three register reads per sample at 1 kHz, that
is roughly one error every 70 ms. The expectation was a few lost or
retried samples. What happened instead:

```
supervisor: Running -> Recovering on Stall (no sample for 3868 ms)
supervisor: Recovering -> Running on Recovered (samples flowing after attempt 1)
```

Acquisition **stopped completely** for almost four seconds, until
device-service's watchdog declared a stall and the supervisor soft-reset
the MCU. One transient error, not a dead link.

## Locating it

The trace had every layer on one clock (`custom_acq_*` tracepoints):

- a `custom_acq_spi_error` on a `FIFO_LEVEL` read;
- the `custom_acq_drain` event for that pass, returning with `err=-5` and
  samples still in the MCU;
- then **no `custom_acq_irq` at all** until the soft reset.

The mechanism is case 05's, reached through a different door. DATA_READY
stays high while the MCU's FIFO is non-empty, but the GPIO interrupt is
edge-triggered. A drain pass that returns early leaves the line high, and
a line that stays high produces no further rising edge. The threaded
handler treated any failed register read as "end of pass", so a single
transient -EIO left data in the FIFO and nothing to wake the driver
again.

The analyzer's first version blamed the **MCU/IRQ layer**, because the
first visible anomaly was an IRQ gap that no drain pass covered. It now
attributes a gap that begins with a pass ending in an error to that
error's layer. The same trace then reads `spi`.

## Fix

`custom_acq_irq_thread()` absorbs transient errors. A failed register
read is retried, and only `DRAIN_MAX_ERRORS` (3) consecutive failures end
the pass. A dead link still ends it, and stays the watchdog's and the
supervisor's job, which is what they are for.

## Verified on the board (1.3.2)

| scenario | before (1.3.1) | after (1.3.2) |
| --- | --- | --- |
| `spi_error` | stall 3.9 s, supervisor recovery, attributed to `mcu_irq` | **no stall, 0 sequence gaps**, no supervisor transition, attributed to `spi` |
| `stall` (the case recovery exists for) | — | 4.26 s without samples → Recovering → Running on the first attempt |
| whole matrix (7 scenarios, `clock_jump` excluded) | 1 of 7 analyzable (trace ring overwritten) | **7/7 attributed to the expected layer** |

The first run also showed that the analysis needs the trace it is
anchored on. With `sched_switch` and `irq_handler_*` enabled, about
430 000 events per scenario overwrote half of an 8 MB ring, including the
inject marker, which left six of seven scenarios unusable. M8 now records
only `custom_acq` events and markers.

## What generalises

**Fault injection finds the bugs that "it works" hides.** In every
earlier run no spontaneous SPI error happened at the wrong moment, so
this path had never run. The first deliberate error found it.
The supervisor recovered the device, which is good, but recovery is the
last line of defence, not the first.
