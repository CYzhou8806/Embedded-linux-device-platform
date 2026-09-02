"""Integration tests against real hardware: MCU + Pi + custom_acq.ko must
already be up (see device-tree/README.md's install/verify steps). Run with
sudo (control is root-only sysfs) - see README.md in this directory.

Design note: these are wall-clock-duration-bounded, not sample-count-bounded,
and check an error-rate bound rather than requiring zero gaps. Empirically
(see docs/debugging/case-06-*.md), MCU sample production/drain-to-kfifo is
bursty - multi-second gaps between bursts have been observed as a normal
characteristic, not a fault - so a fixed sample-count loop has unpredictable
wall-clock time, and a "zero gaps" assertion is unrealistic for any consumer
slower than the MCU's peak burst rate (Python very much included; even the
C++ device-service has documented drops under sustained load). This matches
what Plan.md actually asks for V5's integration layer: verify the sequence
mechanism and check the error rate, not assert perfection.
"""
import time

import pytest

from acq_device import AcqStall

KNOWN_DEVICE_ID = 0xAC00ACC0
KNOWN_FW_VERSION = 0x00010300

RUN_DURATION_S = 5.0
# Generous: covers the multi-second bursty-production gaps seen in
# practice (case-06) without mistaking them for a genuine stall.
STALL_TIMEOUT_S = 8.0
# Loose ceiling - just needs to catch a real regression (e.g. every
# sample being a gap), not police normal Python-consumer packet loss.
MAX_GAP_RATE = 0.5


def _read_for_duration(acq, duration_s):
    """Reads samples until duration_s elapses. Returns (samples, gaps).
    Fails the test (not raises) on an AcqStall - see AcqStall's docstring."""
    __tracebackhide__ = True
    samples = 0
    gaps = 0
    last_seq = None
    t0 = time.monotonic()
    while time.monotonic() - t0 < duration_s:
        try:
            seq, _value = acq.read_sample(timeout_s=STALL_TIMEOUT_S)
        except AcqStall as e:
            pytest.fail(
                f"{e} - see docs/debugging/case-06-spi-controller-stall-under-sustained-load.md"
            )
        if last_seq is not None:
            expected = (last_seq + 1) & 0xFFFFFFFF
            if seq != expected:
                gaps += 1
        last_seq = seq
        samples += 1
    return samples, gaps, last_seq


def test_device_online(acq):
    assert acq.device_id() == KNOWN_DEVICE_ID
    assert acq.fw_version() == KNOWN_FW_VERSION


def test_sequence_continuity(acq):
    acq.start()

    samples, gaps, _last_seq = _read_for_duration(acq, RUN_DURATION_S)

    assert samples > 0, f"read zero samples in {RUN_DURATION_S}s"
    gap_rate = gaps / samples
    assert gap_rate <= MAX_GAP_RATE, (
        f"{gaps}/{samples} samples were gaps ({gap_rate:.0%}), over the "
        f"{MAX_GAP_RATE:.0%} ceiling"
    )


def test_kfifo_overflow_growth_is_bounded(acq):
    acq.start()

    before = acq.kfifo_overflow()
    samples, _gaps, _last_seq = _read_for_duration(acq, RUN_DURATION_S)
    after = acq.kfifo_overflow()

    delta = after - before
    # Same reasoning as MAX_GAP_RATE above: some overflow under a slow
    # consumer is expected, not a regression by itself. A regression of
    # docs/debugging/case-05-*.md would look like massively more overflow
    # than samples actually delivered, not merely "some".
    assert delta <= max(samples, 1) * 50, (
        f"kfifo_overflow rose by {delta} while only {samples} samples were "
        f"read in {RUN_DURATION_S}s - disproportionate to what a slow "
        "consumer alone would explain; possible regression of "
        "docs/debugging/case-05-irq-thread-stale-fifo-level-snapshot.md"
    )


def test_stop_start_resets_sequence(acq):
    acq.start()
    # Advance the sequence well past 0 before stopping, so the next
    # start()'s "back to 0" isn't just a no-op first-ever-start.
    _samples, _gaps, last_seq = _read_for_duration(acq, RUN_DURATION_S)
    assert last_seq is not None and last_seq > 0
    acq.stop()

    # stop() doesn't clear the kernel kfifo, only halts production - any
    # samples still queued from before the stop would otherwise be read
    # back first and mistaken for "didn't reset". Drain them out (a short
    # timeout, not STALL_TIMEOUT_S, since "no more data" is the expected
    # end condition here, not a stall).
    try:
        while True:
            acq.read_sample(timeout_s=0.5)
    except AcqStall:
        pass

    # MCU firmware resets seq_counter/FIFO on control=1 (see case-05 /
    # Watchdog's soft-reset design) - a fresh start should restart at 0.
    acq.start()
    try:
        restarted_seq, _value = acq.read_sample(timeout_s=STALL_TIMEOUT_S)
    except AcqStall as e:
        pytest.fail(
            f"{e} - see docs/debugging/case-06-spi-controller-stall-under-sustained-load.md"
        )

    assert restarted_seq == 0, f"expected sequence to restart at 0 after {last_seq}, got {restarted_seq}"
