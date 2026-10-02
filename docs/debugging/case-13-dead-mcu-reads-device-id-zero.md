# Case 13: A dead MCU answers "DEVICE_ID = 0x00000000", and the echo check lets it through

**Platform:** Raspberry Pi 5, `device-platform-image-ab` (kernel 6.12.93), MCU firmware v1.4, custom-acq driver
**Occurred:** 2026-10-01, the board freshly power-cycled; found while starting M5 (machine state orchestration)

## Observation

Reading the driver's sysfs attributes right after the Pi came up:

```
device_id=0x00000000
fw_version=cat: read error: Input/output error
sample_rate=cat: read error: Input/output error
kfifo_overflow=0
```

device-service was stuck in `activating`. A power cycle of the Pi
stopping the MCU is known from earlier sessions, and the fix is a reset
of the MCU. What was new is the asymmetry: one register reads
successfully as zero, the others fail. Nothing about a dead MCU makes
register 0 special, so the difference had to be on the Pi's side.

## Mechanism

A register read is two SPI frames: the address, then a NOP. The value
comes back in the reply to the second frame, together with an echo of
the address that the driver checks
([`custom_acq.c`](../../driver/custom-acq/custom_acq.c), `custom_acq_reg_read()`):

```c
if (rx[0] != addr) {          /* echo mismatch */
	ret = -EIO;
	...
}
```

An MCU that is not driving MISO reads as all zeros. For `fw_version`
(address `0x01`) the echo is `0x00 != 0x01`, so the read gets -EIO, which
is correct. For `device_id` the address **is** `0x00`, the echo of a dead
bus matches it, and the read "succeeds" with value `0x00000000`. The
check that was supposed to separate "the MCU answered" from "the bus is
floating" is blind for exactly one register, and that register is the
one everything uses as a liveness probe.

## Who was fooled

- **device-service's startup probe** read `device_id` and logged
  `device online: DEVICE_ID=0x00000000`, then carried on. On this board it
  failed a step later anyway: on the A/B image, MCU authentication runs
  before device-service starts. On the development image nothing else
  would have stopped it.
- **The old Watchdog's recovery** probed with the same read, and treated
  "didn't throw" as "the MCU responds, try a soft reset". On a dead MCU it
  would have soft-reset into nothing forever.
- **Not fooled: the A/B health check.** It compares `device_id` against
  the expected value `0xac00acc0`, not against "readable". That is why
  this boot was correctly left uncommitted.

## Fix

The protocol can't be fixed from the Pi side alone. Echoing `~addr`, or
a fixed non-zero marker, would need a firmware change. So userspace stops
trusting the probe's success and checks its value:

- startup rejects `0x00000000` and `0xffffffff` (a bus floating the other
  way): `"DEVICE_ID=0x00000000 - MCU not responding (reset it: tools/mcu-reset.sh)"`;
- the M5 Supervisor records the ID read at startup and compares every
  recovery probe against it ("probe read DEVICE_ID=0x00000000, expected
  0xac00acc0 - MCU not responding"), so a dead MCU uses up the recovery
  attempts and latches `Fault` with evidence. The fault evidence records
  every sysfs attribute separately for this reason: `device_id=0x00000000`
  next to `fw_version=error: ...` is the signature;
- the driver carries a comment at the echo check explaining the blind
  spot.

`tests/test_supervisor.cpp`'s `DeadMcuLatchesFaultWithEvidence`
reproduces the board's state (a zero `device_id`, a failing `fw_version`)
and checks all three. The MCU itself was not reset in this session, so the
fix has not yet been exercised on the board.

## What generalises

**A liveness check has to test for a value the dead state cannot produce.**
"The read succeeded" is not such a value whenever the dead state reads as
zeros and zero is a legitimate answer somewhere in the protocol. The
health check got this right by accident of design: it compares against
the expected ID because it was written to detect the *wrong* MCU, and so
it also detects *no* MCU.
