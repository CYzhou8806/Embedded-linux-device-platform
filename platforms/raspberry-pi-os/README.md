# Platform: Raspberry Pi OS on Raspberry Pi 5

The control group. Same board, same MCU, same source — but a full
Debian trixie install instead of a minimal Yocto image, and vendor
kernels from `apt` instead of self-built ones.

It exists to separate two things that a single platform cannot: how much
of the observed latency comes from the **kernel**, and how much from the
**image** around it.

## Bring-up status

| | |
| --- | --- |
| Distribution | Raspberry Pi OS, Debian 13 "trixie", on its own SD card |
| Stock kernel | `6.18.39+rpt-rpi-2712` (`apt`) |
| RT kernel | `linux-image-6.18.39+rpt-rpi-v8-rt` — Raspberry Pi's **officially maintained** `PREEMPT_RT` flavour, same version number as the stock one |
| Driver | builds out-of-tree against both kernels' headers; `DEVICE_ID = 0xac00acc0` verified on both |
| Service | `device-service` cross-built and run unchanged |
| Middleware | the static `devbus` binaries from the Yocto cross-build container run here as-is; no rebuild needed |

## How things get built here

Unlike the Yocto card, this one has a **native toolchain**. The driver
builds against `raspberrypi-kernel-headers`, the service builds with the
distro's `g++`, and installing an entire RT kernel is one `apt install`
away — which is exactly why the RT comparison in M1 was done here first
rather than on the image that has to keep working.

## Differences that bite

- **`/boot/firmware/`, not `/boot/`.** Config, kernels, overlays and
  `cmdline.txt` all live one directory deeper than on the Yocto card, so
  any deployment script written for one needs the path adjusted.
- **Kernel selection.** M1 used `config.txt`'s `kernel=` line, which is
  *persistent*: a bad kernel stays selected across reboots. The Yocto
  card's one-shot `tryboot` mechanism is the safer route and is what
  this platform's experiments use from here on — `kernel=` is left
  alone.
- **A busy system.** journald writes to disk, the package manager and a
  full systemd service set are present, and the background is
  desktop-grade. That is the point of this platform, not a defect.

## What this platform established

Measured 2026-09-20, with the MCU powered and the driver rebuilt
natively against the stock kernel.

### The ~650 samples/s puzzle was a module parameter

M1 ([case 07](../../docs/debugging/case-07-preempt-rt-comparison-exposes-a-different-bottleneck.md))
recorded that this card sustained only ~640-655 samples/s where the
Yocto card reached ~1000/s, with the same driver and MCU, and left the
cause open. It was the driver's `inter_frame_us` module parameter: at
the default of 100 µs this card delivers 686/s, at 90 µs it delivers
1070/s with nothing dropped. **A cliff between two adjacent settings**,
not a property of the card. Past it the MCU's FIFO never empties, the
threaded IRQ handler never returns, and the hard-IRQ timestamp every
latency measurement depends on goes stale — which is what made the
planned metric unusable here. The full sweep and what it overturns is in
that case's follow-up.

### PREEMPT_RT's benefit replicated on a kernel nobody here built

p99.9 for an untuned consumer under load, vendor kernels:
**3 959 µs → 38 µs**, against 3 988 µs → 34 µs for the self-built pair
on the Yocto card. Two independent kernel pairs, two distributions,
within a few percent. The RT median penalty replicated as well
(6.9 µs vs. 4.2 µs idle).

### The image contributes almost nothing to latency

The whole reason for this platform. Same workload, same board, both
6.18 non-RT: this full Debian install and the minimal Yocto image differ
by **0.7%** on p99.9 under load, with overlapping maxima. For this
workload the kernel accounts for nearly all of the latency and the
distribution around it for nearly none.

### Full isolation is not reachable from `apt`

Both vendor kernels are built without `CONFIG_NO_HZ_FULL` and
`CONFIG_RCU_NOCB_CPU`. `isolcpus` works; `nohz_full=` and `rcu_nocbs=`
are logged once at boot and ignored, and `/sys/devices/system/cpu/nohz_full`
does not exist. Getting the complete isolation configuration on a Pi 5
means building the kernel — which is what the
[Yocto card](../yocto-scarthgap/) does.

This also reproduced [case 09](../../docs/debugging/case-09-preempt-rt-busy-spin-starves-rcu.md)'s
RCU starvation exactly while the measured latency stayed clean, which
split that case's conclusion in two.

## How experiments are run here

```bash
experiments/rt-kernel/run_on_pios.sh stock|v8|rt
```

One-shot `tryboot` per kernel, `cmdline_iso.txt` for isolation, a
`devbus-bootcheck.service` that reboots if the network is gone three
minutes after boot, and `panic=10`. `config.txt` is never modified — M1
used its persistent `kernel=` line, which is the riskier route and is
not used any more.

## Results from this platform

- [`results/devbus/pi5-raspios/`](../../results/devbus/pi5-raspios/) —
  one directory per kernel, `e2e/` for the full-chain run, `ifus-sweep/`
  for the parameter sweep, `rt_matrix.png`.
- [`docs/devbus-experiments.md`](../../docs/devbus-experiments.md) —
  "Raspberry Pi 5, Raspberry Pi OS" and "The two platforms side by side".
