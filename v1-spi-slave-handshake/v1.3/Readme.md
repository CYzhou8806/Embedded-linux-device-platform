# V1.3 — FIFO + Interrupt SPI + Continuous Acquisition

## What This Version Does

V1.3 turns the MCU from a passive command responder into an active data producer. The device now generates data at a configurable sample rate, buffers it internally, and lets the host read it on demand — just like a real acquisition instrument.

Three changes from V1.2:

1. **SPI switched from blocking to interrupt mode** — the MCU no longer stalls waiting for the host. It can produce data and respond to commands concurrently.
2. **Timer-driven data generation** — a hardware timer triggers at the configured sample rate, producing a new sample each tick and pushing it into a ring buffer (FIFO).
3. **DATA_READY GPIO** — a hardware signal that goes high when the FIFO has data, allowing future versions to use interrupt-driven reads instead of polling.

## New Registers

| Address | Name             | R/W | Description                                    |
|---------|------------------|-----|------------------------------------------------|
| 0x05    | `FIFO_LEVEL`     | RO  | Number of samples currently in the FIFO        |
| 0x06    | `DATA_SEQ`       | RO  | Sequence number of oldest sample (peek, no pop)|
| 0x07    | `DATA_VAL`       | RO  | Value of oldest sample (pops after read)       |
| 0x08    | `OVERFLOW_COUNT` | RO  | Cumulative count of dropped samples            |

## Architecture

Two interrupts run independently on the MCU:

```
Timer Interrupt (TIM2)              SPI Interrupt (SPI2)
triggered every 1/SAMPLE_RATE s     triggered when Pi sends a frame
        |                                   |
        v                                   v
  generate sample                    parse command from rx_buf
        |                                   |
        v                                   v
  fifo_push() → write to head       reg_read/reg_write
        |                              (may call fifo_pop()
        v                               → read from tail)
  update DATA_READY GPIO                    |
                                            v
                                     fill tx_buf with response
                                     re-arm SPI for next frame
```

The FIFO is lock-free: the timer only writes `fifo_head`, the SPI callback only writes `fifo_tail`. No shared variable is modified by both sides, so no interrupt disabling is needed.

## Verification Results

- **100 Hz, 10 seconds**: 1000 samples received, 0 sequence gaps, 0 overflow — perfect acquisition
- **Protocol stress test**: 10,000 frames, 0 echo mismatches, 792 frames/sec
- **1000 Hz**: SPI remains stable (verified by register reads during acquisition), but Python/spidev polling cannot keep up — the host reads ~200 samples/sec while 1000/sec are produced, causing expected overflow. This throughput ceiling motivates the V3 kernel driver with GPIO interrupt-driven reads.

## Key Engineering Decisions

**Lock-free FIFO instead of critical sections**: the initial design used a shared `fifo_count` variable incremented by the timer ISR and decremented by the SPI ISR, protected by `__disable_irq()` / `__enable_irq()`. This caused SPI overrun (OVR) because HAL interrupt-mode SPI fires a per-byte interrupt — a 5-byte frame requires 5 consecutive SPI interrupts. Disabling interrupts during `fifo_push()` blocks one or more byte-level SPI interrupts; the SPI DR register is overwritten before HAL reads it, corrupting the internal byte counter and permanently misaligning all subsequent frames. The lock-free redesign removes `fifo_count` entirely: the timer ISR writes only `fifo_head`, the SPI ISR writes only `fifo_tail`, and the count is derived from `head - tail`. On Cortex-M3, aligned 16-bit reads and writes are atomic (single-instruction), so no interrupt masking is needed.

**Interrupt priority**: SPI2 is set to preemption priority 1 (higher), TIM2 to priority 3 (lower). This ensures SPI frame handling is never delayed by timer processing.

**Polling throughput ceiling**: at ~200 samples/sec effective read rate, the spidev polling approach reaches its limit around 200–300 Hz sample rate. Higher rates require kernel-level SPI and interrupt-driven reads (V3).

## What Changed from V1.2

| Aspect         | V1.2                        | V1.3                                    |
|----------------|-----------------------------|-----------------------------------------|
| SPI mode       | Blocking (HAL polls)        | Interrupt (callback-driven)             |
| Data source    | None                        | Timer-generated, configurable rate      |
| Buffering      | None                        | 32-entry lock-free ring buffer          |
| Data readout   | N/A                         | SEQ peek + VAL pop protocol             |
| Host signal    | None                        | DATA_READY GPIO                         |
| Concurrency    | Single-threaded             | Two independent interrupt sources       |

