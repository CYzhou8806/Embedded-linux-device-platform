# Case 04: Concurrent SPI Callers Interleaved Mid-Protocol, Corrupting the Two-Frame Read/Write

**Platform:** STM32F103VET6 (SPI slave) + Raspberry Pi 5, `driver/custom-acq/custom_acq.c` (Linux kernel driver, host side)
**Occurred:** V3, right after adding the GPIO threaded IRQ handler that drains the MCU's FIFO

## Symptom

Writing to the new `control` sysfs attribute (`echo 1 > .../control`, to start acquisition) intermittently failed with `-EIO`. `dmesg` showed:

```
custom-acq spi0.0: echo mismatch writing reg 0x83: got 0x05
custom-acq spi0.0: echo mismatch reading reg 0x05: got 0x7f
custom-acq spi0.0: IRQ: failed to read FIFO level: -5
```

`0x83` is `REG_CONTROL | CMD_WRITE_FLAG` — the command byte the write should have echoed back. `0x05` is `REG_FIFO_LEVEL`, a completely different register, read by a completely different code path (the IRQ thread).

## Expected

The driver's register protocol is pipelined: a logical read or write is two back-to-back SPI transfers (address/command frame, then a NOP frame that collects the previous frame's echoed response), separated by `INTER_FRAME_US` (500µs) so the MCU's ISR has time to prepare the reply. `custom_acq_reg_read()`/`custom_acq_reg_write()` were expected to behave as atomic operations from the driver's point of view — call one, get back a consistent result.

## Investigation

The two register addresses in the error message were the giveaway: `control_store()` (triggered by the `echo` from userspace) writes `REG_CONTROL`; `custom_acq_irq_thread()` (triggered by the GPIO interrupt that `echo 1` itself causes, once acquisition starts and the MCU's timer produces the first sample) reads `REG_FIFO_LEVEL`. These are two different call paths running on two different kernel threads, and nothing prevented them from both trying to talk to the same SPI device at the same time.

`spi_sync_transfer()` is only atomic for a *single* transfer at the SPI controller level — it does not know or care that our protocol needs two specific transfers to land back-to-back with nothing else interleaved in between. Between `custom_acq_xfer(spi, cmd, val, rx)` (frame 1) and `custom_acq_xfer(spi, CMD_NOP, 0, rx)` (frame 2) — separated by a 500µs sleep, plenty of scheduling time — nothing stopped a second SPI "logical operation" from a different thread from sending its own address frame in that gap.

## Root Cause

The write's sequence was interleaved by the IRQ thread's read:

1. `control_store()` sends frame 1: `0x83` (write REG_CONTROL, start).
2. `control_store()` sleeps 500µs (`INTER_FRAME_US`).
3. In that gap, `custom_acq_irq_thread()` (already running — the write's own side effect, starting acquisition, immediately produced the first sample and fired the interrupt) sends its own frame 1: `0x05` (read REG_FIFO_LEVEL).
4. `control_store()` wakes up and sends what it believes is its NOP/echo-collection frame — but the MCU's reply queue is now one frame ahead of what `control_store()` expects, because the IRQ thread's frame 1 was inserted in between. `control_store()` receives `0x05` (an echo of the IRQ thread's read address, not its own write command) instead of `0x83`.
5. Every frame after that point is off by one relative to what each caller expected, corrupting both operations — a classic frame-desync symptom, structurally similar to Case 02's SPI overrun (different root cause, same category of "one side's timing assumption doesn't hold under concurrency").

No SPI bytes were physically lost or corrupted here (unlike Case 02's OVR) — the bug was purely on the host (Linux) side: two logically-separate protocol conversations were allowed to interleave on the wire.

## Fix

Added a `struct mutex spi_lock` to `struct custom_acq`, held for the full duration of `custom_acq_reg_read()`/`custom_acq_reg_write()` — both frames and the sleep between them — so a "logical register operation" is now atomic with respect to any other caller, not just each individual `spi_sync_transfer()`:

```c
static int custom_acq_reg_read(struct spi_device *spi, u8 addr, u32 *val)
{
	struct custom_acq *priv = spi_get_drvdata(spi);
	...
	mutex_lock(&priv->spi_lock);
	/* both frames + the inter-frame sleep happen here */
	mutex_unlock(&priv->spi_lock);
	return ret;
}
```

A mutex (not a spinlock) is correct here because the critical section sleeps (`usleep_range()` and the SPI transfer itself can both block).

## Verification

Re-ran the same start/stop acquisition burst that reproduced the bug: `kfifo_level` went from `0` to `1` with no echo-mismatch errors in `dmesg`, and repeated bursts stayed clean. The race is inherently timing-dependent, so this isn't an airtight proof it can never recur under different timing — but the fix addresses the actual mechanism (unserialized concurrent access to a stateful multi-frame protocol), not just this specific reproduction.
