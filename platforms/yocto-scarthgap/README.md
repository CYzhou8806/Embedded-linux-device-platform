# Platform: Yocto Scarthgap on Raspberry Pi 5

The project's main line. A custom `bitbake`-built image containing the
kernel driver, the Device Tree overlay, the C++ device service and
enough networking to be reachable — nothing else.

## Bring-up status

| | |
| --- | --- |
| Image | `device-platform-image`, Yocto Scarthgap, `MACHINE=raspberrypi5` |
| Stock kernel | `6.6.63-v8-16k` (meta-raspberrypi BSP), `CONFIG_PREEMPT` |
| Driver | `custom-acq.ko`, built in-image as a Yocto recipe, `/dev/acq0` |
| Overlay | `custom-acq.dtbo`, compiled from `device-tree/custom-acq-overlay.dts` |
| Service | `device-service`, systemd unit, starts at boot |
| MCU link | verified: `DEVICE_ID = 0xac00acc0`, 1000 Hz sustained |
| Middleware | `devbus` runs here, but is **not** in the image yet — static binaries are copied to `/tmp` by hand |

## How things get built

Everything except `devbus` is a recipe in
[`yocto/meta-device-platform/`](../../yocto/meta-device-platform/):

```bash
bitbake device-platform-image
```

Each recipe points back at the single copy of the source in this repo
through `FILESEXTRAPATHS`, so there is no second copy to keep in sync.

`devbus` has no recipe yet, and **the target has no compiler**, so it is
cross-compiled to a static binary in a container whose glibc matches
Scarthgap:

```bash
podman build -t devbus-xbuild experiments/rt-kernel/
# -> ~/devbus-xbuild/out/{devbus-bench,devbus-tests,devbus-ls,acq-bridge,...}
```

The result is fully static, which is also why the same binaries run
unmodified on [Raspberry Pi OS](../raspberry-pi-os/).

## Deployment

- Image components: reflashed, or hot-swapped for iteration (`scp` the
  `.ko`, `rmmod`/`insmod`, restart the unit).
- `devbus` binaries: `scp` to `/tmp`. **`/tmp` is cleared on reboot**, so
  they have to be re-copied after every kernel test.

## Self-built kernels and the tryboot safety net

Two extra kernels are built and booted on this card without replacing
the stock one — same `rpi-6.18.y` source, same `bcm2712_defconfig`, the
only difference being `CONFIG_PREEMPT_RT`:

| name | preemption | release |
| --- | --- | --- |
| stock | `PREEMPT` | `6.6.63-v8-16k` |
| std | `PREEMPT` | `6.18.52-devbus-std-iso+` |
| rt | **`PREEMPT_RT`** | `6.18.52-devbus-rt-iso+` |

Four independent layers keep the card recoverable: each test kernel in
its own `/boot/<name>/` directory selected by `os_prefix=`; started only
through the firmware's **one-shot `tryboot`**, so any reboot returns to
stock; `panic=10`; and a `devbus-bootcheck.service` that reboots if the
gateway is unreachable three minutes after boot. Full description and
the build steps: [`experiments/rt-kernel/README.md`](../../experiments/rt-kernel/README.md).

One-command run of a whole kernel's experiment matrix:

```bash
experiments/rt-kernel/run_on_pi.sh std|rt|stock
```

## Known limits of this platform

- **No compiler, no package manager.** Anything new has to be
  cross-compiled elsewhere or added as a recipe.
- **~33 MB free on root.** Kernel module staging installs only the
  modules the Pi actually loads (51 modules, 2.5 MB instead of ~100 MB).
- **`/var/log` is a tmpfs**, so a crash loses the journal. The
  boot-check service dumps to the FAT `/boot` partition for that reason.
- **WiFi (`wlan0`) is the only link**, which makes "no network" the same
  observable event as "the kernel didn't boot" — deliberately, since the
  boot-check service reboots on exactly that.
- Kernel command-line isolation flags are **silently ignored** when the
  matching `CONFIG_` isn't set; always read them back from
  `/sys/devices/system/cpu/nohz_full` ([case 09](../../docs/debugging/case-09-preempt-rt-busy-spin-starves-rcu.md)).
- The governor must be set through
  `/sys/devices/system/cpu/cpufreq/policy0/`; the per-CPU path returns
  EIO on the Pi 5.

## Results from this platform

- [`results/devbus/pi5-yocto/`](../../results/devbus/pi5-yocto/) — per-kernel
  CSVs, `cyclictest` histograms, `dmesg`, `host.txt` with the exact
  command line and governor state; `e2e/` holds the full-chain run.
- [`docs/devbus-experiments.md`](../../docs/devbus-experiments.md) — "Raspberry Pi 5:
  three kernels" and "End to end on real hardware".
- [`docs/performance.md`](../../docs/performance.md) — driver and service
  latency/throughput characterization.
- Case studies found here: [case 08](../../docs/debugging/case-08-busy-spin-subscriber-hit-by-rt-throttling.md),
  [case 09](../../docs/debugging/case-09-preempt-rt-busy-spin-starves-rcu.md).
