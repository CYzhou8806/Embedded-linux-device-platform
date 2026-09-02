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
- 2026-08-31 (later same day): before starting V3's second sub-milestone
  (GPIO threaded IRQ), wired DATA_READY (MCU `PA8` → Pi `GPIO17`) and
  verified it end to end. Full writeup, including the debugging methodology
  (why the first two verification attempts were inconclusive/risky, and
  what worked instead) is
  `docs/debugging/case-03-data-ready-gpio-verification.md`. Short version:
  found and fixed a real firmware bug (a leftover per-frame debug GPIO
  toggle was masking the real level-based DATA_READY logic, which itself
  was never called from the FIFO-push path). Added `control`/`fifo_level`/
  `data_val` sysfs attributes to `driver/custom-acq/custom_acq.c` to drive
  real acquisition for the test. After the firmware fix, `gpiomon` showed
  exactly one rising edge (data arrives) and one falling edge (FIFO
  drained) — correct semantics for an interrupt-driven consumer. Clears
  the way for the actual GPIO IRQ driver code.
- 2026-08-31 (later still): V3's second and third sub-milestones (GPIO
  threaded IRQ, kfifo) done and verified on hardware. Overlay now
  describes `data-ready-gpios` on the `custom-acq` node; the driver
  requests it via `gpiod_get`/`gpiod_to_irq`/`devm_request_threaded_irq`,
  and on each interrupt drains the MCU's hardware FIFO into a kernel
  `kfifo` (new `kfifo_level`/`kfifo_overflow` sysfs attributes; no
  `/dev/acq0` yet to read the samples out to userspace). Found and fixed
  a real concurrency bug along the way — see
  `docs/debugging/case-04-spi-transaction-race-two-frame-protocol.md`:
  the two-frame pipelined register protocol wasn't safe against the IRQ
  thread and a sysfs write racing on the same SPI bus, corrupting frame
  sequencing. Fixed with a per-device mutex around each full
  read/write operation. Verified clean after the fix: `kfifo_level`
  correctly reflects drained samples with no echo-mismatch errors.
- 2026-09-02: V3's fourth and final sub-milestone (`/dev/acq0`) done and
  verified on hardware — all four V3 sub-milestones now complete.
  Registered a misc device backed by a `file_operations` table
  (`open`/`read`/`poll`/`release`), wiring userspace syscalls to the
  driver's `kfifo`. Added a `wait_queue_head_t`: the IRQ thread calls
  `wake_up_interruptible()` after draining new samples; `read()` blocks
  via `wait_event_interruptible()` when empty (`-EAGAIN` under
  `O_NONBLOCK`); `poll()` registers on the same queue via `poll_wait()`
  and reports `EPOLLIN` when non-empty. `fifo_lock` was converted from
  `spinlock_t` to `struct mutex` in the process, since
  `kfifo_to_user()`'s internal `copy_to_user()` can page-fault (sleep),
  which isn't legal under a spinlock; both producer and consumer only
  ever run in process context, so the switch has no downside.

  Hit one build error along the way: `devm_misc_register()` doesn't
  exist in this kernel (mis-remembered — no devm-managed variant of
  `misc_register()` exists upstream either). Fixed with plain
  `misc_register()` plus `devm_add_action_or_reset()` to register a
  manual cleanup action, same effect as the other `devm_*` resources.

  Hardware tests all passed: clean `dmesg` across `insmod`/`rmmod`,
  `/dev/acq0` appears/disappears correctly; `read()` blocks correctly
  before data arrives and returns correctly after (verified the
  blocking path doesn't busy-loop by killing a blocked `dd` with
  `timeout`); `poll()` times out before acquisition starts and reports
  `EPOLLIN` immediately after; `kfifo_overflow` stayed 0 throughout.
  Plan.md's stated V3 completion criteria (clean `modprobe`, `/dev/acq0`
  appears, `read()` gets data, `poll()` blocks/wakes correctly) are all
  satisfied.
- 2026-09-02 (later): V4 Phase 1 (`userspace/device-service/`, a minimal
  end-to-end C++ acquisition service) done. Testing it against sustained
  acquisition (not just V3's own short bursts) turned up a real driver
  bug — see
  `docs/debugging/case-05-irq-thread-stale-fifo-level-snapshot.md`.
  Short version: `custom_acq_irq_thread()`'s drain loop only read
  `REG_FIFO_LEVEL` once before looping, then decremented a local
  counter instead of re-checking the MCU's real state each iteration.
  DATA_READY is level-driven but the GPIO IRQ is edge-triggered, so as
  long as the line stayed high only one interrupt ever fired; once MCU
  production (~1kHz) outpaced the protocol's read rate, the stale
  snapshot went out of date within the first iteration, the loop
  returned early, and the MCU's own hardware FIFO silently overflowed
  from then on with no further interrupt to wake the drain loop back
  up. None of V3's own testing (short start/stop bursts) ran long
  enough to expose this. Fixed by re-reading `REG_FIFO_LEVEL` on every
  loop iteration and only exiting once the MCU genuinely reports empty;
  throughput on the same sustained-load test went from 1 sample per run
  to 2176 samples in ~5s, sequence numbers continuous, `kfifo_overflow`
  and `dmesg` both clean.

  Also added `tools/mcu-reset.sh`: a purely software-triggered MCU
  hardware reset via the debugger's SWD link (bypasses SPI entirely),
  for silencing sustained SPI bus activity ("buzzing") after a test run
  without touching the physical reset button. Verified against a live
  case: `kfifo_overflow` was still climbing by several hundred per
  second before the reset, completely flat for 2s immediately after.
