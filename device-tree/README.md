# custom-acq Device Tree overlay — deployment log

Target: Raspberry Pi 5 (`RaspberryPi5`, 192.168.178.172), Raspberry Pi OS
(Debian 13 "trixie"), kernel `6.18.39+rpt-rpi-2712`.

## What this overlay does

`custom-acq-overlay.dts` targets `spi0` CS0 (`/dev/spidev0.0`), which is
currently claimed by the stock `spidev` driver (confirmed via
`/sys/bus/spi/devices/spi0.0/of_node/compatible` → `spidev`, base dtb has
labels `spi0` and `spidev0` in `/proc/device-tree/__symbols__/`, same
pattern the official `mcp2515-can0.dts` overlay uses):

- fragment@0: sets the existing `&spidev0` node's `status = "disabled"` —
  frees up CS0 so no two drivers fight over the same chip select.
- fragment@1: adds a new child node under `&spi0` with
  `compatible = "edp,custom-acq"`, `reg = <0>` (CS0), 1 MHz max SPI clock
  (matches `RaspPi/testv13.py`'s `spi.max_speed_hz = 1000000`).

Net effect after applying: `/dev/spidev0.0` disappears, a new SPI device
`spi0.0` appears bound to nothing until `driver/custom-acq/custom_acq.ko`
is loaded and matches on the `edp,custom-acq` compatible string.

## Build

```bash
dtc -@ -I dts -O dtb -o custom-acq.dtbo custom-acq-overlay.dts
```

`-@` emits `__symbols__` so the overlay can resolve `&spi0`/`&spidev0`
against the running system's base dtb. Already done once on the Pi —
produced a 678-byte `custom-acq.dtbo`, no warnings.

## Install (not yet done — this is the exact command sequence)

```bash
# 1. back up config.txt so this is a one-line revert
sudo cp /boot/firmware/config.txt /boot/firmware/config.txt.bak-pre-custom-acq

# 2. install the compiled overlay
sudo cp ~/device-tree/custom-acq.dtbo /boot/firmware/overlays/

# 3. enable it (appends one line)
echo 'dtoverlay=custom-acq' | sudo tee -a /boot/firmware/config.txt

# 4. apply — overlays are only (re)loaded at boot
sudo reboot
```

**Effect on the running system**: `/dev/spidev0.0` goes away after
reboot (any script hardcoding `spi.open(0, 0)` via `spidev` breaks until
reverted). `/dev/spidev0.1` (CS1) is untouched.

## Verify after reboot

```bash
ls /sys/bus/spi/devices/                                   # spi0.0 should still be listed
cat /sys/bus/spi/devices/spi0.0/of_node/compatible          # expect: edp,custom-acq
cat /sys/bus/spi/devices/spi0.0/modalias                    # should no longer say spidev
sudo dmesg | tail -20                                        # look for the disabled spidev0 node, no probe yet (driver not loaded)

# load the driver (not auto-loaded yet — out-of-tree module, manual insmod during dev)
cd ~/custom-acq && sudo insmod custom_acq.ko
sudo dmesg | tail -5            # expect: "custom-acq bound, DEVICE_ID=0x..."
cat /sys/bus/spi/devices/spi0.0/device_id      # our sysfs attribute
cat /sys/bus/spi/devices/spi0.0/fw_version
```

## Revert

```bash
sudo cp /boot/firmware/config.txt.bak-pre-custom-acq /boot/firmware/config.txt
sudo rm /boot/firmware/overlays/custom-acq.dtbo
sudo reboot
```

`/dev/spidev0.0` comes back, `driver/custom-acq` no longer has anything to
bind to.

## Log

- 2026-08-28: `custom_acq.ko` built and insmod/rmmod tested clean against
  the running kernel (headers matched exactly, no cross-compile needed).
  `custom-acq.dtbo` built clean. Neither installed yet — waiting on
  confirmation before touching `config.txt` / rebooting the Pi.
- 2026-08-31: overlay installed and Pi rebooted. `spi0.0` now reports
  `compatible = edp,custom-acq`, `/dev/spidev0.0` is gone,
  `/dev/spidev0.1` untouched. First `insmod` attempt was run with the MCU
  unpowered: `probe()` succeeded but `dmesg` showed `DEVICE_ID=0x00000000`
  and `cat .../fw_version` returned `Input/output error` — a floating-MISO
  false positive (address 0x00 happens to equal the expected echo of 0x00,
  so the read "succeeds" with a garbage value; any nonzero register
  address correctly fails the echo check). Powered the MCU and retried
  without reloading the module:
  ```
  $ cat /sys/bus/spi/devices/spi0.0/device_id
  0xac00acc0
  $ cat /sys/bus/spi/devices/spi0.0/fw_version
  0x00010300
  ```
  Both match the values hardcoded in
  `v1-spi-slave-handshake/v1.3/MCU_v1-MCU-device-control/Core/Src/main.c`
  (`REG_DEVICE_ID` → `0xAC00ACC0`, firmware version → `0x00010300` for
  V1.3). This confirms the full chain end to end: Device Tree overlay →
  kernel `probe()` → real SPI transaction → correct MCU register data —
  the first V3 milestone (Plan.md "第一版") is done.
