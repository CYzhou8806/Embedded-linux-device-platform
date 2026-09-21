# PREEMPT_RT kernels for the Pi 5 Yocto image, tested without risking the card

This builds two kernels from **the same source and the same config**
(`raspberrypi/linux` `rpi-6.18.y`, `bcm2712_defconfig`, 16K pages, HZ=250).
They differ only in the preemption model:

| name | preemption | release string |
| --- | --- | --- |
| `std` | `CONFIG_PREEMPT` (Raspberry Pi default) | `6.18.52-devbus-std+` |
| `rt` | `CONFIG_PREEMPT_RT` (mainline since 6.12) | `6.18.52-devbus-rt+` |

Both are booted next to the image's own stock kernel (`6.6.63-v8-16k`),
without replacing it. Results: [`docs/devbus-experiments.md`](../../docs/devbus-experiments.md),
section "Raspberry Pi 5: PREEMPT_RT".

## How the card is protected

- **Each test kernel lives in its own boot-partition directory** (`/boot/std/`,
  `/boot/rt/`) and is selected with the firmware's `os_prefix=`. Kernel,
  DTB, overlays and `cmdline.txt` all come from that directory. The stock
  kernel's files are never touched.
- **Test kernels are started only through tryboot**
  (`systemctl reboot --reboot-argument="0 tryboot"` with an `os_prefix`
  line in `/boot/tryboot.txt`). Tryboot is one-shot, so any reboot, for any
  reason, lands back on the stock kernel.
- **`panic=10`** on the test kernels' command line. A kernel panic reboots
  after 10 s, which means back to stock.
- **`devbus-bootcheck.service`** (installed on the target). On a test
  kernel it dumps `dmesg` + journal to `/boot/devbus-boot-<release>.log`.
  If the gateway isn't reachable 3 minutes after boot, it reboots, which
  again means back to stock. `/var/log` on this image is tmpfs, so the dump
  on the FAT boot partition is the only log that survives.

## Steps

```sh
bash build_kernels.sh ~/devbus-xbuild                      # ~25 min per kernel on 12 cores
ssh pi "lsmod | awk 'NR>1{print \$1}'" > ~/devbus-xbuild/loaded-modules.txt
scp pi:/boot/overlays/custom-acq.dtbo ~/devbus-xbuild/
for v in std rt; do
  podman run --rm -v ~/devbus-xbuild:/w:Z -v "$PWD/../..":/src:ro,Z devbus-xbuild bash /w/stage_kernel.sh $v
done
# copy stage-<v>/boot/<v> -> /boot/<v>,  stage-<v>/lib/modules/* -> /lib/modules/
# /boot/<v>/cmdline.txt = stock cmdline + "isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3 irqaffinity=0,1 panic=10"
# /boot/tryboot.txt     = /boot/config.txt + "[all]\nos_prefix=<v>/"
ssh pi 'systemctl reboot --reboot-argument="0 tryboot"'
```

`stage_kernel.sh` installs only the modules the Pi actually loads, plus
their dependencies (51 modules, 2.5 MB instead of ~100 MB). The Yocto
rootfs has about 35 MB free. It also builds the out-of-tree
`custom-acq` driver against each kernel.

## Things that went wrong on the way (so they don't again)

1. **WiFi didn't come up on 6.18.** `brcmf_fwvid_request_module: mod=cyw:
   failed`. Since 6.x, brcmfmac loads a per-vendor sub-module at runtime
   (`brcmfmac-cyw` for the Pi 5's CYW43455). "Install what 6.6 had
   loaded" missed it, because 6.6 loaded `brcmfmac_wcc`. The boot-check
   service rebooted the Pi back to stock, and its log on `/boot` showed
   the error. Fix: always stage all `brcmfmac-*` vendor modules.
2. **Setting the governor through `cpuN/cpufreq/scaling_governor` fails
   with EIO** on the Pi 5. All four cores share `policy0`; write
   `/sys/devices/system/cpu/cpufreq/policy0/scaling_governor` instead.
3. The kernel modules are XZ-compressed (`CONFIG_MODULE_COMPRESS_XZ`).
   The image's kmod supports it (`+XZ`), but module-name matching must
   strip `.ko.xz`.
