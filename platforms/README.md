# Platforms

The same MCU, the same Raspberry Pi 5, and the same source tree, running
on **two different Linux distributions** and, between them, **four
kernels**. Nothing in this directory duplicates code: the driver, the
device service and `devbus` are built from one source. What differs is
how they are built, which kernel they run under, and what else is
running on the system.

## Why two systems

The project's main line is a custom Yocto image, because that is what an
embedded device actually ships: a minimal, reproducible,
bitbake-described root filesystem. But a minimal image is also a very
quiet one, and quiet systems make latency numbers look better than they
are. A full Raspberry Pi OS install — systemd services, a package
manager, a desktop-grade background — is the honest control group.

So the two platforms answer one question that neither could answer
alone: **of the latency this platform shows, how much comes from the
kernel, and how much comes from the image around it?**

A second, narrower use: Raspberry Pi OS ships an
officially-maintained `PREEMPT_RT` kernel through `apt`, so it can check
the self-built RT kernel on the Yocto card against a vendor build of the
same preemption model.

## The two of them side by side

| | [Yocto Scarthgap](yocto-scarthgap/) | [Raspberry Pi OS](raspberry-pi-os/) |
| --- | --- | --- |
| **Build** | `bitbake` cross-build of the whole image; driver and service are Yocto recipes. No toolchain on the target — `devbus` is cross-compiled statically in a container (`experiments/rt-kernel/Containerfile`). | Native `gcc` on the target; kernel modules build against `raspberrypi-kernel-headers`. The same static binaries from the container also run here unchanged. |
| **Kernels** | Stock `6.6.63-v8-16k` from the BSP, plus two self-built `6.18.52` kernels from one source and one config differing only in `CONFIG_PREEMPT_RT` (`experiments/rt-kernel/`). Booted one-shot via `tryboot`. | Vendor kernels from `apt`: `6.18.39+rpt-rpi-v8` and the official RT flavour `6.18.39+rpt-rpi-v8-rt`. |
| **System environment** | BusyBox userland, ~33 MB free on root, `/var/log` is a tmpfs, WiFi is the only link, almost no background services. | Debian trixie: full systemd service set, journald on disk, package manager, desktop-grade background load. |

## Where the numbers live

| Platform | Results | Write-up |
| --- | --- | --- |
| Dev host (x86-64 Ubuntu, for reference only) | [`results/devbus/dev-host/`](../results/devbus/dev-host/) | [`docs/devbus-experiments.md`](../docs/devbus-experiments.md) |
| Yocto Scarthgap on the Pi 5 | [`results/devbus/pi5-yocto/`](../results/devbus/pi5-yocto/) | [`docs/devbus-experiments.md`](../docs/devbus-experiments.md), [`docs/performance.md`](../docs/performance.md) |
| Raspberry Pi OS on the Pi 5 | [`results/devbus/pi5-raspios/`](../results/devbus/pi5-raspios/) | [`docs/devbus-experiments.md`](../docs/devbus-experiments.md), [case 07](../docs/debugging/case-07-preempt-rt-comparison-exposes-a-different-bottleneck.md) |

## What running both actually produced

Measured 2026-09-20, after the same workload had been run on both:

- **The image barely matters for latency.** Same board, same workload,
  both on a 6.18 non-RT kernel: a full Debian install and a minimal
  Yocto image differ by **0.7%** on p99.9 under load. A stripped image
  earns its keep on boot time, size, attack surface and
  reproducibility — on this evidence, not on latency.
- **PREEMPT_RT's benefit is a property of the preemption model, not of
  one build.** An untuned consumer's p99.9 under load fell 3 988 → 34 µs
  on kernels built here, and 3 959 → 38 µs on Raspberry Pi's own
  kernels. So did the cost: RT's idle median is ~2 µs worse on both.
- **Full core isolation needs a self-built kernel.** The vendor kernels
  have no `CONFIG_NO_HZ_FULL` or `CONFIG_RCU_NOCB_CPU`, so `nohz_full=`
  and `rcu_nocbs=` are silently dropped. `isolcpus` alone is what `apt`
  can give you.
- **Two earlier conclusions of this project were corrected** by running
  them on the second card: case 07's unexplained ~650 samples/s (a
  module parameter one step past a cliff) and case 09's 130 ms RCU stall
  cost (the starvation was real; the milliseconds were the serial
  console printing the report).

A result produced on one machine has been observed, not tested. Two of
the four findings above only appeared because there was a second one.
