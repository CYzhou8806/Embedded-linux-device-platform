# Tests (Plan.md V5)

Three layers, per Plan.md's V5 milestone:

- **C++ unit tests** — not here. They live in
  `userspace/device-service/tests/` (GoogleTest, `ctest`-driven), not a
  top-level `tests/unit/`. `device-service/` already had its own test
  suite from V4; moving it into a top-level layout is deliberately
  deferred to V8 polish, same as the `firmware/` rename and the other
  directory-naming cleanup — see `CLAUDE.md`.
- **`integration/`** — Python/pytest against real hardware (MCU + Pi +
  `custom_acq.ko`). See `integration/README.md`.
- **`hardware/`** — a standalone stress-test script, not a pytest suite.
  See `hardware/README.md`.

## Known limitation (applies to all three real-hardware layers)

Sustained SPI traffic can wedge the Pi's SPI controller badly enough to
need a physical MCU reset — see
`docs/debugging/case-06-spi-controller-stall-under-sustained-load.md`. Both
`integration/` and `hardware/` are built to detect this (a bounded
per-read stall timeout) and fail/report cleanly rather than hang, but
recovering from an actual stall still needs a human at the hardware.
