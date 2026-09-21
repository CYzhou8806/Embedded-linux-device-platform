# Case 08: The Fastest Configuration Had the Worst Tail: RT Throttling on an Isolated Core

**Platform:** Raspberry Pi 5 (Yocto Scarthgap image), `userspace/devbus`, `bench/run_rt_matrix.sh`
**Occurred:** V2 (`Plan.md` §12.4, M4 + M1): the first run of the PREEMPT_RT comparison matrix on the target

## Symptom

The matrix measures devbus one-way latency (64-byte samples at 1 kHz,
60 000 samples per configuration). A background `stress-ng` load runs on
cores 0-1, while cores 2-3 are isolated (`isolcpus=2,3 nohz_full=2,3`).
The most aggressive configuration, a busy-spinning subscriber at
`SCHED_FIFO` 80 pinned to isolated core 3, had the best median by far and
by far the worst tail:

| config (stock 6.6 kernel, under load) | p50 | p99 | max |
| --- | --- | --- | --- |
| futex wait, SCHED_FIFO 80, isolated core | 2.5 µs | 7.6 µs | 92 µs |
| **busy-spin**, SCHED_FIFO 80, isolated core | **0.24 µs** | **39 453 µs** | **51 452 µs** |

A subscriber that never sleeps, alone on its own core, had a p99 four
orders of magnitude worse than one that sleeps in the kernel.

## Narrowing it down

- **It wasn't noise.** It reproduced in a second full run with the same
  shape: p99 ~39 ms, max ~51 ms.
- **The numbers had a shape.** p99 ≈ 39 ms and max ≈ 51 ms at 1 kHz. About
  1 % of samples waited tens of milliseconds, capped a little above 50 ms.
  A random scheduler hiccup wouldn't have a hard ceiling. Something was
  taking the CPU away for a fixed ~50 ms window, often enough to catch
  1 % of samples.
- **50 ms per second is a documented kernel number.**
  `/proc/sys/kernel/sched_rt_runtime_us` = 950000 of
  `sched_rt_period_us` = 1000000: real-time tasks may use at most 95 % of
  each second. The remaining 50 ms is reserved for non-RT tasks, so a
  runaway `SCHED_FIFO` loop can't lock up the machine. A busy-spinning
  `SCHED_FIFO` task uses 100 % of its core, so it is throttled for 50 ms of
  every second. Samples arriving in that window wait until it ends.
- **The kernel said so.** `dmesg` on the target:
  ```
  [  371.133076] sched: RT throttling activated
  [  501.389658] sched: RT throttling activated
  ```

## Confirming the root cause

Same run, same load, same pinning. The only change was
`echo -1 > /proc/sys/kernel/sched_rt_runtime_us` (throttling off) for the
duration of the spin run:

| config (stock 6.6 kernel, under load) | p50 | p99 | max |
| --- | --- | --- | --- |
| busy-spin, throttling on (default) | 0.24 µs | 39 453 µs | 51 452 µs |
| busy-spin, **throttling off** | 0.26 µs | **0.28 µs** | **0.93 µs** |

The tail disappeared completely: the worst of 60 000 samples arrived in
under a microsecond.

## It depends on the kernel version

The same matrix on a 6.18 kernel (same card, same load, same pinning,
throttling left at its default of 950000) showed no stall at all:

| busy-spin, SCHED_FIFO 80, isolated core, throttling **on** | p99 | max |
| --- | --- | --- |
| 6.6.63 (stock Yocto) | 39 453 µs | 51 452 µs |
| 6.18.52 (`CONFIG_PREEMPT`) | 0.28 µs | 1.06 µs |

Since 6.12, the blanket 95 % RT cap has been replaced by a **fair-server
(deadline server)** mechanism. CFS tasks on a CPU get their share of time
only when they are actually runnable on that CPU. An isolated core has no
CFS work, so nothing takes the core away from the spinner. The takeaway
isn't "throttling is gone". It's that **the same user-space tuning
produces different worst cases on different kernel versions**. A latency
claim is only as good as the kernel version written next to it.

(On the PREEMPT_RT build of the same 6.18 source, busy-spinning had a
different problem: RCU callback starvation. That's
[case 09](case-09-preempt-rt-busy-spin-starves-rcu.md).)

## Where else it showed up

In the slow-subscriber isolation experiment, the `Block` policy makes
the **publisher** spin, at `SCHED_FIFO` 80, while waiting for a full
subscriber queue. The fast subscriber's max latency in that run was
52.5 ms on 6.6 (53.8 ms in the first run). That is the same ~50 ms
signature, this time on the producer side. On 6.18 the same run's max
was 5.0 ms, which is just the configured `block_timeout`. This is concrete evidence for the devbus roadmap item of making
`Block` wait on a futex instead of spinning.

## Takeaways

- **RT throttling is a safety net for general-purpose systems.** It is
  the right default, because a runaway RT task would otherwise hang the
  machine. On an isolated core dedicated to one RT task, though, it
  protects nothing and just injects 50 ms stalls. It's also
  system-wide (`sched_rt_runtime_us`), not per core, so disabling it is
  a deliberate, documented decision about the whole device.
- **Never spin at an RT priority on a non-isolated core with throttling
  disabled.** That combination is how you get a hard hang. The safe
  combinations are: spin at `SCHED_OTHER` on an isolated core; spin at
  RT priority on an isolated core with throttling off; or don't spin and
  use futex wait, which, as the matrix shows, already gets within a few
  microseconds.
- **Bimodal latency with a flat ceiling has a mechanism behind it.** A
  number like "about 50 ms, never more" should send you to look for a
  timer, a quota, or a period, not for random noise.
