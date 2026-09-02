import subprocess
import sys

import pytest

from acq_device import AcqDevice


@pytest.fixture
def acq():
    """Opens /dev/acq0 for the test, guarantees acquisition is stopped
    afterwards even if the test raises - a test that leaves the MCU
    acquiring would corrupt the next test's sequence-continuity checks.

    stop() itself can hit the case-06 SPI stall (see acq_device.AcqStall's
    docstring) - if that happens here in teardown there's nothing more
    this fixture can do about it (a stuck kernel thread needs a physical
    MCU reset, not a Python-level retry), so it prints a loud warning
    instead of letting a teardown failure mask the test's own result.
    """
    dev = AcqDevice()
    dev.open()
    try:
        yield dev
    finally:
        try:
            dev.stop()
        except subprocess.TimeoutExpired:
            print(
                "\nWARNING: stop() timed out - the SPI bus may be stalled "
                "(see docs/debugging/case-06-*.md). A physical MCU reset "
                "is likely needed before the next test run.",
                file=sys.stderr,
            )
        finally:
            dev.close()
