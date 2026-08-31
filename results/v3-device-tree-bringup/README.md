# V3 Device Tree + kernel driver bring-up — raw console log

`console-log-2026-08-31.txt` is the unedited terminal output from the first
successful bring-up of the custom-acq kernel driver on Raspberry Pi 5,
captured with `set -x` so every command run is visible alongside its output.

What it shows, in order:
1. Target system: kernel `6.18.39+rpt-rpi-2712`, Debian 13.
2. `spi0.0` bound to the `custom-acq` driver (`edp,custom-acq` compatible
   string, matching `device-tree/custom-acq-overlay.dts`) instead of the
   stock `spidev`.
3. `dmesg` output including the historical
   `echo mismatch reading reg 0x01: got 0x00` line from the first `probe()`
   attempt — that happened because the MCU was unpowered at the time (see
   `device-tree/README.md` log for the diagnosis).
4. `device_id` / `fw_version` sysfs reads after powering the MCU:
   `0xac00acc0` / `0x00010300`, matching the constants hardcoded in
   `v1-spi-slave-handshake/v1.3/MCU_v1-MCU-device-control/Core/Src/main.c`.

This is the artifact for the V3 first-milestone claim ("Device Tree overlay
+ from-scratch kernel driver, verified against real hardware") — full
narrative context lives in `device-tree/README.md`.
