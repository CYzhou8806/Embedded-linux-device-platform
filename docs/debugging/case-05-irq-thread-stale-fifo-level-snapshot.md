# Case 05: IRQ Drain Loop Stopped After One Stale FIFO_LEVEL Snapshot

**Platform:** STM32F103VET6 (SPI slave) + Raspberry Pi 5, `driver/custom-acq/custom_acq.c` (Linux kernel driver, host side)
**Occurred:** V4 Phase 1, while first testing the new C++ device service (`userspace/device-service/`) against a sustained (~1kHz) acquisition run instead of the single-sample bursts V3's own testing used

## Symptom

The new C++ service (`Device`/`AcquisitionWorker`/`RingBuffer`, reading `/dev/acq0`) reported exactly **one sample** per run, no matter how long acquisition was left running:

```
device online: DEVICE_ID=0xac00acc0 FW_VERSION=0x00010300
seq=0 value=0

received signal 15, shutting down...
shutdown report: samples_read=1 gap_count=0 kfifo_overflow=0
```

`kfifo_overflow` (the kernel driver's own overflow counter) stayed at `0`, so it looked like nothing was being lost — but a direct sysfs probe told a different story:

```
$ echo 1 > control; sleep 2
$ cat fifo_level      # MCU hardware FIFO
31
$ cat kfifo_level      # kernel-side kfifo (userspace-facing buffer)
1
$ cat kfifo_overflow
0
```

The MCU's own hardware FIFO (`FIFO_DEPTH = 32`) was stuck full at `31`, while the kernel's kfifo — the thing `/dev/acq0` actually serves reads from — had a single stale sample sitting in it. Data was disappearing somewhere in between, silently, without tripping any counter this driver exposes.

## Expected

V3's stated design (`custom_acq_irq_thread()`, added when the GPIO threaded IRQ was written): DATA_READY is a level signal — high whenever the MCU's FIFO is non-empty — and one rising-edge interrupt is expected to drain *all* pending samples, looping "until the MCU reports empty," not just some fixed number captured at interrupt time.

## Investigation

The firmware's timer (`TIM2`, `Core/Src/tim.c`) is configured for a 1ms period — `reg_control & 0x01` gates a `fifo_push()` on every tick, so once acquisition starts the MCU should be producing roughly 1000 samples/sec. That doesn't match "the driver drained exactly one sample and then nothing else, ever" — unless the IRQ thread itself only ran its drain loop once and then never got triggered again.

That pointed straight at `custom_acq_irq_thread()`:

```c
static irqreturn_t custom_acq_irq_thread(int irq, void *data)
{
	...
	ret = custom_acq_reg_read(priv->spi, REG_FIFO_LEVEL, &level);
	...
	while (level > 0) {
		ret = custom_acq_read_sample(priv->spi, &s);
		...
		drained++;
		level--;
	}
	...
}
```

`level` is read from the MCU **once**, before the loop starts, then only ever decremented locally — it's a snapshot, not a live value. The loop exits once that snapshot count of samples has been drained, regardless of how many *more* samples the MCU produced in the meantime.

And the IRQ itself is edge-triggered (`IRQF_TRIGGER_RISING` in `probe()`), while DATA_READY is level-driven (verified back in Case 03). That combination means: the driver only ever gets **one** rising edge for as long as the MCU's FIFO stays continuously non-empty. If production outpaces drain — and it does, badly: each drained sample costs two register reads (`REG_FIFO_LEVEL`, then the peek/pop pair for `REG_DATA_SEQ`/`REG_DATA_VAL`), each a two-frame SPI op with a 500µs inter-frame gap, so roughly 2–3ms per sample against the MCU's 1ms production interval — DATA_READY never drops back to low. No second edge ever arrives to re-trigger the thread. The MCU's hardware FIFO fills to `FIFO_DEPTH - 1` and stays there, silently dropping every subsequent sample via its own `fifo_overflow++` (a counter this driver has never read or exposed — see the sysfs attribute table in Case 04/`docs/learning-qa.md` Q25 for what *is* exposed).

## Root Cause

`custom_acq_irq_thread()`'s drain loop relies on a one-time snapshot of `REG_FIFO_LEVEL` instead of re-checking the MCU's real FIFO state on every iteration. Under light, bursty load (the only scenario V3's own testing exercised — a handful of samples, then stop) the snapshot and reality never diverge enough to matter. Under sustained load, the snapshot goes stale within the first iteration or two, the loop exits early, and — because DATA_READY is level-driven but the IRQ registration is edge-triggered — nothing ever re-triggers the thread to finish the job.

## Fix

Replaced the one-time snapshot with a loop that re-reads `REG_FIFO_LEVEL` from the MCU on every iteration and only exits once the MCU itself reports `0`:

```c
for (;;) {
	ret = custom_acq_reg_read(priv->spi, REG_FIFO_LEVEL, &level);
	if (ret) { ... break; }
	if (level == 0)
		break;

	ret = custom_acq_read_sample(priv->spi, &s);
	...
	drained++;
}
```

This costs one extra `REG_FIFO_LEVEL` read per drained sample (three two-frame SPI ops per sample now, instead of two — see the file's inline comment for the full reasoning), but it means the thread keeps draining for as long as data keeps arriving, and only returns once DATA_READY's underlying condition (`fifo_head == fifo_tail` on the MCU) is genuinely true — matching the level semantics DATA_READY was designed around, instead of assuming the edge that triggered it is still an accurate picture of the world.

## Verification

Same sustained ~1kHz acquisition run, before and after, using the new C++ service as the test harness:

| | before | after |
|---|---|---|
| samples delivered in ~5s | 1 | 2176 |
| `gap_count` (sequence continuity) | 0 (nothing to check — only 1 sample) | 0 |
| `kfifo_overflow` | 0 | 0 |
| `dmesg` | clean | clean |

Sequence numbers were continuous across the whole run (`seq` incrementing by exactly 1, no gaps), confirming the fix isn't just "reading more bytes" but genuinely keeping up with the MCU's real production rate. Effective throughput (~435 samples/sec against a ~1000/sec MCU rate) is still below the MCU's raw rate — the two-frame, echo-verified SPI protocol has real per-sample overhead — but that's now an honest, visible bottleneck rather than a silent one masked by an early-exiting drain loop. Characterizing and improving that gap is exactly what Plan.md's V7 (end-to-end latency/throughput analysis) exists for, not something to chase down further here.

**Update, 2026-09-04 (V7):** characterized and largely closed. The gap was 3 register reads per sample (`REG_FIFO_LEVEL`+`REG_DATA_SEQ`+`REG_DATA_VAL`, `REG_FIFO_LEVEL` being this fix's own re-check cost), each dominated by the driver's `inter_frame_us` gap (500us at the time this case was written). A hardware sweep of that value found a non-monotonic relationship (200-300us was worse than either 500us or lower) and settled on a new default of 100us, reaching ~1000 samples/sec (matching the MCU's apparent production rate) with `kfifo_overflow`/sequence gaps mostly at 0 — see `docs/session-log.md`'s 2026-09-04 fourth-round entry and `docs/debugging/case-06-*.md`'s updated "Next steps" for the full data.

## Lesson

This bug was invisible to every V3-era test because none of them exercised sustained load — every prior test started acquisition, grabbed a sample or two, and stopped, which never gave the stale-snapshot assumption enough time to diverge from reality. It took building an actual consumer (`AcquisitionWorker`, V4 Phase 1) that runs acquisition continuously for several seconds to expose it. A useful reminder for the rest of V4/V5: the userspace service and its future stress tests (Plan.md V5's `tests/hardware/`) aren't just downstream consumers of a finished driver — they're the first realistic sustained-load test the driver has ever seen.
