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

### The SDK

`bitbake -c populate_sdk device-platform-image` produces a standard SDK:
a self-extracting installer with the cross toolchain and a **sysroot that
matches the image exactly** — same glibc, same library versions, same
compiler flags.

```bash
./poky-glibc-x86_64-device-platform-image-cortexa76-raspberrypi5-toolchain-5.0.20.sh -y -d ~/sdk/device-platform
. ~/sdk/device-platform/environment-setup-cortexa76-poky-linux   # sets CC/CXX, --sysroot, OE's CMake toolchain file
cmake -S userspace/devbus -B build-devbus -G Ninja && cmake --build build-devbus
```

| | |
| --- | --- |
| First SDK build | 49 min (on a machine that had already built the image) |
| Rebuild after changing its contents | 5.6 min |
| Installer / installed | 254 MB / 1.7 GB |
| devbus / device-service, incl. unit tests | 7 s / 11 s |
| Unit tests of the cross-built binaries | 16/16 and 28/28 pass under user-mode QEMU (`qemu-aarch64 -L <sysroot>`, Yocto's own `qemu-native`) — no board needed |

Two things had to change before the SDK could build this repository:

- **The sysroot only contains `-dev` packages of what is installed in the
  image.** nlohmann-json is header-only and GoogleTest is test-only, so
  neither is ever installed, and the first SDK could build devbus but not
  device-service. `TOOLCHAIN_TARGET_TASK:append = " nlohmann-json-dev
  googletest-dev"` in the image recipe adds build-only dependencies.
- **`gtest_discover_tests()` ran the test binary at build time** to list
  its tests — an aarch64 binary on an x86 host. `DISCOVERY_MODE PRE_TEST`
  defers that to `ctest`, which runs where the binary can run.

Compared with the podman container above: the SDK's sysroot is the image's
own, so a binary built with it links against exactly what the target has,
dynamically, and picks up the image's hardening flags
(`-fstack-protector-strong -D_FORTIFY_SOURCE=2 -mbranch-protection=standard`,
full RELRO) — nothing assembled by hand. The container is lighter (no
Yocto build needed) and its static binaries run on both distributions,
which is why the kernel experiments kept using it.

`populate_sdk_ext` builds the **extensible SDK** instead: it carries a
BitBake environment and sstate, and adds `devtool` — `devtool modify
device-service` checks the recipe's source out into a workspace,
`devtool build` / `devtool deploy-target` build it and copy it to the board
over SSH, and `devtool finish` turns the changes into patches or a
bbappend in a layer. The standard SDK is for application developers who
only need to compile against the image; the eSDK is for people who
change recipes without a full Yocto setup of their own.

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
