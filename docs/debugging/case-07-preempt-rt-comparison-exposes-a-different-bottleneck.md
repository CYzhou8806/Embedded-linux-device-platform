# Case 07: PREEMPT_RT Comparison Exposes a Different Bottleneck Than the One It Was Built to Test

**Platform:** STM32F103VET6 (SPI slave) + Raspberry Pi 5, `driver/custom-acq/custom_acq.c` + `userspace/device-service`, tested on a second SD card running stock Raspberry Pi OS (Debian 13 "trixie") rather than the project's main Yocto image
**Occurred:** V2 (`Plan.md` §12.4, M1) — the PREEMPT_RT vs. stock-kernel comparison this project's decision gate called for

This case doesn't end with "RT helped" or "RT didn't help" as a clean
headline result, even though that's the question it set out to answer.
It ends with the planned measurement method (`docs/performance.md`'s
per-sample IRQ-to-userspace latency percentiles) turning out to be
invalid on this specific card, a real root cause for *that* found and
confirmed, a substitute metric (throughput + sequence-loss rate) used
instead, and that substitute metric answering the original RT question
cleanly — just not with the numbers this doc's title might suggest.

## Setup

M1's plan (`private/next-steps.md`, `Plan.md` §12.4) was: install
Raspberry Pi's official PREEMPT_RT kernel flavor on a spare Raspberry Pi
OS install (not the main Yocto image, to avoid revalidating
`driver`/overlay against a rebuilt Yocto kernel before knowing whether RT
even helps), redeploy the current driver and `device-service` unchanged,
and rerun `docs/performance.md`'s scheduler comparison methodology —
same `SCHED_FIFO`(80)/`cpu_affinity_core`(3)/`mlockall` combination,
only varying whether the kernel is PREEMPT_RT.

`linux-image-6.18.39+rpt-rpi-v8-rt` matched the currently-running stock
kernel's exact version number (`6.18.39+rpt-rpi-2712`), installed
alongside it via `apt` (old kernel kept, selectable via
`/boot/firmware/config.txt`'s `kernel=` line, for a bounded-risk
rollback). Driver and `device-service` both cross-built cleanly against
both kernels' headers; `DEVICE_ID` sysfs readback (`0xac00acc0`)
confirmed the SPI handshake worked on both.

## The metric broke before any comparison could happen

The first run (`baseline_noload`, no tuning, no background load — the
easiest possible case) produced nonsensical numbers:

```
median_us   = 12841959.2   # ~12.8 seconds
p99_us      = 25485856.3   # ~25.5 seconds
```

The raw CSV showed why: `irq_ts_ns` (the driver's hard-IRQ timestamp,
from `ktime_get_ns()` in `custom_acq_irq_hard()`) was **identical across
the entire 20-second run** — one value, shared by all ~16,000-30,000
samples. `/proc/interrupts` confirmed it: the `custom-acq` IRQ's count
had incremented by only 1 (sometimes low single digits) for the whole
run.

This isn't a driver bug — it's documented, intentional behavior that
just doesn't apply the way it did before. `custom_acq_irq_thread()`:

```c
/* Re-read REG_FIFO_LEVEL from the MCU on every iteration rather than
 * snapshotting it once before the loop. DATA_READY is level-driven
 * but the GPIO IRQ is edge-triggered (IRQF_TRIGGER_RISING) - we only
 * get one rising edge for the whole time the MCU's FIFO stays
 * non-empty. ... Re-checking the real level keeps this thread draining
 * for as long as data keeps arriving, exiting only once the MCU
 * actually reports empty.
 */
for (;;) {
        ret = custom_acq_reg_read(priv->spi, REG_FIFO_LEVEL, &level);
        ...
        if (level == 0)
                break;
        ...
}
```

`docs/performance.md`'s clean ~950us median depended on the MCU's
hardware FIFO regularly hitting empty — many short drain batches, each
getting its own fresh `irq_ts_ns`. On this card, the FIFO essentially
never empties: one continuous drain batch runs for most or all of a
20-second test, so nearly every sample shares one very stale
`irq_ts_ns`, and "IRQ-to-userspace latency" degenerates into "how long
ago the one drain batch that's still running started" — a number that
trivially grows to the length of the whole run and carries no
information about per-sample delay.

## Root cause: this card's achievable SPI throughput is below the MCU's production rate

Computing actual delivered rate directly from `recv_ts_ns` (not the
broken latency field) instead:

| Run | delivered rate | sequence loss |
|---|---|---|
| stock kernel, no load, no tuning | 642.3/s | 36.0% |
| stock kernel, load, no tuning | 656.3/s | 34.5% |
| stock kernel, load, `SCHED_FIFO`+affinity+`mlockall` | 654.6/s | 34.3% |

The MCU produces at a fixed 1000Hz (`reg_sample_rate` default,
`main.c`). This card sustains only ~640-656/s — a genuine ~34-36%
throughput deficit, not a measurement artifact. `docs/performance.md`'s
Yocto-image measurements reached ~1000/s cleanly with zero loss under
the same `inter_frame_us=100` default this card is also running
(confirmed via `/sys/module/custom_acq/parameters/inter_frame_us`).
Since the MCU is producing faster than this card can drain it, the
driver's hardware FIFO is kept permanently non-empty by design — which
is exactly the precondition for the broken-latency-metric symptom
above. The two findings are the same root cause, not two separate bugs.

**The loss pattern itself pointed at a specific structure**: sequence
gaps recur roughly every ~128 delivered samples, each losing ~70-90 in a
row.

```c
#define SAMPLE_KFIFO_SIZE	128
...
DECLARE_KFIFO(samples, struct custom_acq_sample, SAMPLE_KFIFO_SIZE);
```

`SAMPLE_KFIFO_SIZE` is 128 — the spacing between loss events matches the
in-kernel ring buffer's exact capacity. This is the textbook signature
of a producer running persistently faster than its consumer against a
fixed-size ring: the buffer fills, wraps, and drops the overflow in a
burst, repeating every time it refills — not random noise, a
deterministic consequence of a sustained rate mismatch.

**What wasn't pinned down**: *why* this card's achievable throughput is
~35% below the Yocto image's, when both are (per `docs/debugging/case-06`)
driving the same `spi_dw` (DesignWare) SPI controller on the same Pi 5
SoC, with the same `inter_frame_us=100` driver default. A full-desktop
Debian trixie install carries far more background services (Bluetooth,
NetworkManager, WiFi power management, etc. — all visible in this card's
`dmesg`) than the project's deliberately minimal Yocto image, which was
the working hypothesis — but see the next section for why the actual RT
comparison data doesn't support "background contention" as the
mechanism, which leaves this specific number still open.

## The RT comparison itself: clean, and negative

With throughput (not the broken per-sample latency) as the metric,
re-running the same three configs on the RT kernel:

| Run | delivered rate | sequence loss |
|---|---|---|
| RT kernel, no load, no tuning | 641.4/s | 33.5% |
| RT kernel, load, no tuning | 641.5/s | 33.6% |
| RT kernel, load, `SCHED_FIFO`+affinity+`mlockall` | 640.3/s | 33.7% |

Stock and RT are indistinguishable — every number across both kernels
and all three tuning configs sits in a 640-656/s / 33.5-36.0% band, with
background load (`yes > /dev/null` ×4) and the full
`SCHED_FIFO`+affinity+`mlockall` combination changing nothing on either
kernel. This directly contradicts the "background service contention"
hypothesis from the section above: if extra background daemons were
stealing CPU from the acquisition thread, `SCHED_FIFO`-80 (well above
any `SCHED_OTHER` daemon's priority) should have been able to reclaim
that time, the way it did in `docs/performance.md`'s original
`yes`-under-load matrix. It didn't move the number at all here.

That leaves the throughput ceiling as **intrinsic per-transaction
pacing** — `custom_acq_reg_read()`/`write()`'s `usleep_range(inter_frame_us,
inter_frame_us + 100)` gap plus real SPI bus transfer time, three
two-frame register operations per sample — rather than time lost to
preemption. A thread that is voluntarily sleeping or waiting on bus
transfer completion isn't being *interrupted*; raising its scheduling
priority doesn't make a `usleep_range()` call return sooner or a SPI
transfer complete faster. `SCHED_FIFO`/CPU affinity/`mlockall` and
PREEMPT_RT all target the same class of problem — losing the CPU to
something else — which is precisely the class of problem this
particular bottleneck isn't.

## Decision (Plan.md §12.4's gate)

> RT 内核如果带来显著增益 ... 再决定要不要把这个升级成本转嫁到 Yocto
> 主线；如果增益有限，这一步到此为止，时间转给 M2。

Measured gain: none, across three configs, two kernels, a consistent
band of numbers. Per the pre-committed decision gate, M1 stops here —
no Yocto kernel upgrade, no further RT investment. This isn't "RT is
useless" as a general claim; it's a negative result for *this specific
bottleneck* (intrinsic SPI pacing), consistent with `Plan.md`'s own
pre-registered expectation ("现有数据显示光 SCHED_FIFO 就已经把负载下
的尾延迟压回 no-load 基线以下 ... RT 内核在这个负载规模下的增益本身
存疑") — this is that expectation confirmed with real A/B data instead
of left as a guess.

## What's not explained

- The ~35% throughput gap between this card (Raspberry Pi OS / Debian
  trixie) and the project's main Yocto image, both reportedly using the
  same `spi_dw` controller and the same driver defaults. Background
  service load was the working hypothesis but the RT/`SCHED_FIFO` data
  above argues against a contention-based explanation. Possible
  remaining candidates, none tested: a difference in `spi_dw` driver
  version/config between the two OS builds, a difference in SPI clock
  rate or DMA vs. PIO transfer mode between the two device tree
  overlays, or some other fixed per-transfer overhead specific to this
  OS's kernel build. Not investigated further — out of scope for M1's
  actual question (does RT help), and M1's own answer doesn't depend on
  resolving it.
- Whether increasing `SAMPLE_KFIFO_SIZE` beyond 128 would reduce the
  loss rate (it would change the *burst size* of each loss event, not
  the ~34% average rate, which is set by the rate mismatch — but not
  verified).

## Current status (2026-09-16)

M1 is closed per the decision gate above. Card restored to its original
boot state (`config.txt`'s `kernel=` override removed, driver module
`rmmod`'d) and kept as a standing second board for future scheduler/OS
comparison experiments — separate from the main Yocto card, which V6/V7
already validated and wasn't touched by any of this. Next up per
`private/next-steps.md`: M0 (overload the existing STM32 link on its own
terms, drop-policy comparison, backpressure), fully independent of this
card or PREEMPT_RT.
