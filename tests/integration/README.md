# Integration tests

Python/pytest, run directly against real hardware (MCU + Pi +
`custom_acq.ko` loaded — see `device-tree/README.md` for the install/verify
steps). Not a mock/simulation layer: these are meant to catch regressions
the C++ unit tests can't, since they exercise the real driver and SPI bus.

## Environment

```bash
sudo apt install -y python3-pytest
```

No pip/venv — Debian 13 (trixie) ships `python3-pytest` in the official
apt repo, so there's no need for a virtualenv on the Pi.

## Running

```bash
cd tests/integration
python3 -m pytest -v
```

Takes roughly 25-30s (each test reads for a bounded 5s window — see the
design note at the top of `test_acquisition.py`), not instant.

`/dev/acq0` and the read-only sysfs attributes (`device_id`, `fw_version`,
`kfifo_overflow`) are world-readable *if* `/dev/acq0`'s permissions haven't
reverted to the misc-device default (`crw-------`, root-only) since the
module was last loaded — there's no udev rule pinning it to `666` yet, so
after any `rmmod`/`insmod` cycle or reboot you may need
`sudo chmod 666 /dev/acq0` once before running these. `control` (the only
start/stop entry point) is root-only sysfs (`--w-------`) regardless —
`acq_device.py` shells out to `sudo tee` just for that write, which needs
passwordless sudo configured for the invoking user (or you'll get an
interactive password prompt mid-test-run).

Each test uses the `acq` fixture (`conftest.py`), which opens `/dev/acq0`
itself and guarantees `stop()` runs in a `finally` even if the test fails —
don't run these concurrently with `device-service` or another consumer of
`/dev/acq0`: the kfifo has exactly one drain path, so two readers split the
samples between them and every sequence-continuity assertion here will spuriously fail.

## Known limitation

Sustained SPI traffic can wedge the Pi's SPI controller badly enough to
need a physical MCU reset to recover — see
`docs/debugging/case-06-spi-controller-stall-under-sustained-load.md`. These
tests detect that (an 8s per-read stall timeout, reported as a clear
failure) rather than hanging forever, but they can't recover from it
automatically. If a run reports a stall, physically reset the MCU before
running anything else against `/dev/acq0`.
