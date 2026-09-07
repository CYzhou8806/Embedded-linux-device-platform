# MCU Firmware — Iteration History

The firmware went through three iterations before reaching its current
form. Only **v1.3** (the version actually used by everything downstream
— the kernel driver, the Yocto image, the performance measurements) is
kept in the working tree. v1.1 and v1.2 are preserved in git history
and tagged for anyone who wants to check out the exact code at each
stage:

```
git checkout v1.1-snapshot -- v1-spi-slave-handshake/v1.1
git checkout v1.2-snapshot -- v1-spi-slave-handshake/v1.2
```

## v1.1 — SPI slave handshake

MCU as SPI slave, Pi as master, reading back a fixed 5-byte device-ID
response. Proved the basic physical/electrical link and framing before
any real protocol existed. Notable finding: direct jumper-wire clips
caused random data corruption that breadboard routing fixed outright —
an early instance of "suspect the physical layer before the code" that
became a running theme for this project.

## v1.2 — Register protocol

Replaced the fixed device-ID response with a real register interface:
5-byte pipelined frames (command in frame N, response in frame N+1,
echo-verified), a register map (`DEVICE_ID`, `FW_VERSION`, `STATUS`,
`CONTROL`, `SAMPLE_RATE`), and latched error flags. Verified with a
10,000-frame stress test (0 echo mismatches, ~790 frames/sec). This is
where the SPI protocol this project still uses was designed.

## v1.3 — FIFO + interrupt-driven acquisition (current)

Turned the MCU from a passive command responder into an active data
producer: a timer ISR generates samples into a lock-free FIFO
independently of the SPI ISR that serves host reads, plus a
`DATA_READY` GPIO for interrupt-driven host reads. This is the version
the kernel driver, Yocto image, and all `docs/performance.md`
measurements are built against — see
[`v1.3/Readme.md`](v1.3/Readme.md) for the full writeup, register map,
and the lock-free-FIFO design rationale.
