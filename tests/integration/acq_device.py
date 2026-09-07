"""Thin wrapper around /dev/acq0 + its sysfs directory, for integration
tests. Deliberately dependency-free (no spidev, no pyserial) - talks
directly to the same two interfaces userspace/device-service/src/device.cpp
does: the misc device for sample data, sysfs for control/status.

Starting/stopping acquisition writes sysfs `control`, which is root-only
(see tests/integration/README.md) - hence the `sudo tee` subprocess calls
instead of a plain open()/write().
"""
from __future__ import annotations

import os
import select
import struct
import subprocess

SAMPLE_STRUCT = struct.Struct("<IIq")  # matches struct custom_acq_sample: u32 seq, u32 value, s64 irq_ts_ns (V7)


class AcqStall(Exception):
    """Raised by read_sample(timeout_s=...) when no data arrived in time.

    Not a normal error path - see docs/debugging/case-06-spi-controller-
    stall-under-sustained-load.md. The kernel's SPI transfer can wedge
    under sustained back-to-back traffic; when that happens nothing short
    of a physical MCU reset recovers it, so tests must detect a stall by
    timeout rather than block forever waiting for data that isn't coming.
    """


class AcqDevice:
    def __init__(self, dev_path: str = "/dev/acq0",
                 sysfs_dir: str = "/sys/bus/spi/devices/spi0.0/"):
        self.dev_path = dev_path
        self.sysfs_dir = sysfs_dir
        self._fd: int | None = None

    def open(self) -> None:
        self._fd = os.open(self.dev_path, os.O_RDONLY)

    def close(self) -> None:
        if self._fd is not None:
            os.close(self._fd)
            self._fd = None

    def __enter__(self) -> "AcqDevice":
        self.open()
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def _write_sysfs(self, name: str, value: str) -> None:
        # control is root-only (see tests/integration/README.md); the test
        # user has passwordless sudo, so this is the same permission model
        # a real deployment's systemd unit would run under (root).
        #
        # timeout=: writing `control` goes through the same spi_sync path
        # that can stall (AcqStall's docstring / case-06) - a bounded
        # timeout here means a stuck stop() during test teardown reports
        # cleanly instead of hanging the whole pytest run.
        subprocess.run(
            ["sudo", "tee", os.path.join(self.sysfs_dir, name)],
            input=value.encode(), stdout=subprocess.DEVNULL, check=True, timeout=10,
        )

    def _read_sysfs(self, name: str) -> str:
        with open(os.path.join(self.sysfs_dir, name)) as f:
            return f.read().strip()

    def start(self) -> None:
        self._write_sysfs("control", "1")

    def stop(self) -> None:
        self._write_sysfs("control", "0")

    def read_sample_full(self, timeout_s: float | None = None) -> tuple[int, int, int]:
        """Reads one sample as (seq, value, irq_ts_ns) - irq_ts_ns is the
        kernel's ktime_get_ns() at the hard-IRQ that drained this sample
        (V7 latency work, driver/custom-acq/custom_acq.c). If timeout_s is
        given, raises AcqStall instead of blocking forever when no data
        shows up in time - see AcqStall's docstring for why that's a real,
        expected failure mode here, not just defensive paranoia."""
        assert self._fd is not None, "call open() first"
        buf = b""
        while len(buf) < SAMPLE_STRUCT.size:
            if timeout_s is not None:
                r, _, _ = select.select([self._fd], [], [], timeout_s)
                if not r:
                    raise AcqStall(
                        f"no data from {self.dev_path} within {timeout_s}s "
                        f"({len(buf)}/{SAMPLE_STRUCT.size} bytes of current sample)"
                    )
            chunk = os.read(self._fd, SAMPLE_STRUCT.size - len(buf))
            if not chunk:
                raise EOFError(f"{self.dev_path} read returned EOF unexpectedly")
            buf += chunk
        return SAMPLE_STRUCT.unpack(buf)

    def read_sample(self, timeout_s: float | None = None) -> tuple[int, int]:
        """Reads one sample as (seq, value) - the pre-V7 shape, kept as the
        default for callers that don't care about the latency timestamp
        (most of tests/integration and tests/hardware)."""
        seq, value, _irq_ts_ns = self.read_sample_full(timeout_s)
        return seq, value

    def device_id(self) -> int:
        return int(self._read_sysfs("device_id"), 0)

    def fw_version(self) -> int:
        return int(self._read_sysfs("fw_version"), 0)

    def kfifo_overflow(self) -> int:
        return int(self._read_sysfs("kfifo_overflow"), 0)

    def spi_rearm_fail(self) -> int:
        """MCU-side counter (Plan.md V7 / case-06 diagnostics)."""
        return int(self._read_sysfs("spi_rearm_fail"), 0)

    def spi_error_count(self) -> int:
        """MCU-side counter (Plan.md V7 / case-06 diagnostics)."""
        return int(self._read_sysfs("spi_error_count"), 0)
