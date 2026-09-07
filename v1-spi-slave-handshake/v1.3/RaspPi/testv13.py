#!/usr/bin/env python3
"""
V1.3 full test script
Covers:
  1. V1.2 regression test (confirm old functionality still works)
  2. FIFO acquisition test (100 Hz, checks sequence-number continuity)
  3. High-speed stress test (1000 Hz, observes data loss)
  4. 10000-frame protocol stress test
"""

import spidev
import time
import random
import sys

# -- constants --
NOP         = 0x7F
WRITE_FLAG  = 0x80
INTER_FRAME = 0.0005  # 500us

# register addresses
DEVICE_ID       = 0x00
FW_VERSION      = 0x01
STATUS          = 0x02
CONTROL         = 0x03
SAMPLE_RATE     = 0x04
FIFO_LEVEL      = 0x05
DATA_SEQ        = 0x06
DATA_VAL        = 0x07
OVERFLOW_COUNT  = 0x08
SPI_REARM_FAIL  = 0x09
SPI_ERROR_COUNT = 0x0A

# -- SPI init --
spi = spidev.SpiDev()
spi.open(0, 0)
spi.max_speed_hz = 1000000
spi.mode = 0

# -- low-level transfer --
def xfer(cmd, data=0):
    tx = [
        cmd & 0xFF,
        (data >> 24) & 0xFF,
        (data >> 16) & 0xFF,
        (data >> 8)  & 0xFF,
         data        & 0xFF,
    ]
    rx = spi.xfer2(tx)
    time.sleep(INTER_FRAME)
    return rx

echo_mismatch_count = 0

def read_reg(addr):
    global echo_mismatch_count
    xfer(addr)
    r = xfer(NOP)
    echo = r[0]
    if echo != addr:
        echo_mismatch_count += 1
        print(f"  !! ECHO mismatch: expected {addr:#04x}, got {echo:#04x}")
    val = (r[1] << 24) | (r[2] << 16) | (r[3] << 8) | r[4]
    return val

def write_reg(addr, val):
    global echo_mismatch_count
    cmd = addr | WRITE_FLAG
    xfer(cmd, val)
    r = xfer(NOP)
    echo = r[0]
    if echo != cmd:
        echo_mismatch_count += 1
        print(f"  !! ECHO mismatch: expected {cmd:#04x}, got {echo:#04x}")

def stop_and_clear():
    """Stop acquisition and clear error flags"""
    write_reg(CONTROL, 0x00)
    write_reg(CONTROL, 0x02)  # CLEAR_FLAGS

# ══════════════════════════════════════════════
#  Part 1: V1.2 regression test
# ══════════════════════════════════════════════
def test_regression():
    print("=" * 50)
    print("Part 1: V1.2 regression test")
    print("=" * 50)

    xfer(NOP)  # flush the pipeline
    stop_and_clear()

    # Test 1: read DEVICE_ID
    val = read_reg(DEVICE_ID)
    ok = val == 0xAC00ACC0
    print(f"  read DEVICE_ID = {val:#010x}  {'OK' if ok else 'FAIL'}")

    # Test 2: read FW_VERSION (should be 0x00010300 on V1.3)
    val = read_reg(FW_VERSION)
    print(f"  read FW_VERSION = {val:#010x}")

    # Test 3: write/read SAMPLE_RATE
    write_reg(SAMPLE_RATE, 5000)
    val = read_reg(SAMPLE_RATE)
    ok = val == 5000
    print(f"  wrote 5000, read back {val}  {'OK' if ok else 'FAIL'}")

    # Test 4: invalid value gets rejected
    write_reg(SAMPLE_RATE, 99999)
    val = read_reg(SAMPLE_RATE)
    ok = val == 5000
    print(f"  wrote 99999, read back {val}  {'OK (rejected)' if ok else 'FAIL'}")
    st = read_reg(STATUS)
    print(f"  STATUS = {st:#010x}  RANGE_ERR={'yes' if st & 0x04 else 'no'}")

    # Test 5: clear flags
    write_reg(CONTROL, 0x02)
    st = read_reg(STATUS)
    ok = (st & 0x06) == 0
    print(f"  STATUS after clear = {st:#010x}  {'OK' if ok else 'FAIL'}")

    # Test 6: invalid address
    val = read_reg(0x42)
    st = read_reg(STATUS)
    ok = (st & 0x02) != 0
    print(f"  read 0x42, CMD_ERR={'yes' if ok else 'no'}  {'OK' if ok else 'FAIL'}")

    # Test 7: START/STOP
    stop_and_clear()
    write_reg(CONTROL, 0x01)
    st = read_reg(STATUS)
    running = (st & 0x01) != 0
    print(f"  RUNNING after START={'yes' if running else 'no'}  {'OK' if running else 'FAIL'}")
    write_reg(CONTROL, 0x00)
    st = read_reg(STATUS)
    stopped = (st & 0x01) == 0
    print(f"  RUNNING after STOP={'no' if stopped else 'still running'}  {'OK' if stopped else 'FAIL'}")

    stop_and_clear()
    print()

# ══════════════════════════════════════════════
#  Part 2: FIFO acquisition test (low rate, expect zero loss)
# ══════════════════════════════════════════════
def test_acquisition_slow():
    print("=" * 50)
    print("Part 2: FIFO acquisition test (100 Hz, 10 sec)")
    print("=" * 50)

    stop_and_clear()

    # set sample rate to 100 Hz
    write_reg(SAMPLE_RATE, 100)
    val = read_reg(SAMPLE_RATE)
    print(f"  SAMPLE_RATE set to {val} Hz")

    # # start acquisition
    # write_reg(CONTROL, 0x01)
    # print("  acquisition started, waiting 1s for the FIFO to fill...")
    # time.sleep(1.0)

    # # read FIFO_LEVEL
    # level = read_reg(FIFO_LEVEL)
    # print(f"  FIFO_LEVEL after 1s = {level}")


    # start reading immediately after START, no more 1s wait
    write_reg(CONTROL, 0x01)
    print("  acquisition started, reading immediately...")

    # begin continuous reads
    total_samples = 0
    seq_gaps = 0
    last_sequence = -1
    t0 = time.time()
    duration = 10.0

    while time.time() - t0 < duration:
        level = read_reg(FIFO_LEVEL)
        if level == 0:
            time.sleep(0.001)  # FIFO empty, wait 1ms
            continue

        # drain everything currently available (still bounded by the
        # outer duration, so a corrupted FIFO_LEVEL read returning a huge
        # value can't make the script spin out of control here)
        while level > 0 and time.time() - t0 < duration:
            seq = read_reg(DATA_SEQ)
            val = read_reg(DATA_VAL)
            total_samples += 1

            if last_sequence >= 0:
                expected = (last_sequence + 1) & 0xFFFFFFFF
                if seq != expected:
                    seq_gaps += 1
                    if seq_gaps <= 5:  # only print the first 5 gaps
                        print(f"    gap: expected {expected}, got {seq}")
            last_sequence = seq

            level = read_reg(FIFO_LEVEL)

    elapsed = time.time() - t0

    # stop acquisition
    write_reg(CONTROL, 0x00)
    overflow = read_reg(OVERFLOW_COUNT)

    print(f"\n  --- results ---")
    print(f"  duration:        {elapsed:.1f} sec")
    print(f"  samples received: {total_samples}")
    print(f"  sequence gaps:   {seq_gaps}")
    print(f"  FIFO overflow:   {overflow}")
    print(f"  effective rate:  {total_samples / elapsed:.1f} samples/sec")

    if seq_gaps == 0 and overflow == 0:
        print(f"  100 Hz, zero loss")
    elif overflow > 0 and seq_gaps == 0:
        print(f"  overflow but no gaps")
    else:
        print(f"  gaps present, needs investigation")

    stop_and_clear()
    print()

# ══════════════════════════════════════════════
#  Part 3: high-speed acquisition test (1000 Hz, loss expected)
# ══════════════════════════════════════════════
def test_acquisition_fast():
    print("=" * 50)
    print("Part 3: high-speed acquisition test (1000 Hz, 5 sec)")
    print("=" * 50)

    stop_and_clear()

    write_reg(SAMPLE_RATE, 1000)
    val = read_reg(SAMPLE_RATE)
    print(f"  SAMPLE_RATE set to {val} Hz")

    write_reg(CONTROL, 0x01)
    print("  acquisition started...")

    total_samples = 0
    seq_gaps = 0
    last_sequence = -1
    t0 = time.time()
    duration = 5.0

    while time.time() - t0 < duration:
        level = read_reg(FIFO_LEVEL)
        if level == 0:
            time.sleep(0.001)
            continue

        while level > 0 and time.time() - t0 < duration:
            seq = read_reg(DATA_SEQ)
            val = read_reg(DATA_VAL)
            total_samples += 1

            if last_sequence >= 0:
                expected = (last_sequence + 1) & 0xFFFFFFFF
                if seq != expected:
                    seq_gaps += 1
            last_sequence = seq

            level = read_reg(FIFO_LEVEL)

    elapsed = time.time() - t0
    write_reg(CONTROL, 0x00)
    overflow = read_reg(OVERFLOW_COUNT)
    rearm_fail = read_reg(SPI_REARM_FAIL)
    error_count = read_reg(SPI_ERROR_COUNT)

    print(f"\n  --- results ---")
    print(f"  duration:            {elapsed:.1f} sec")
    print(f"  samples received:    {total_samples}")
    print(f"  sequence gaps:       {seq_gaps}")
    print(f"  FIFO overflow:       {overflow}")
    print(f"  SPI re-arm failures: {rearm_fail}")
    print(f"  SPI error callbacks: {error_count}")
    print(f"  effective rate:      {total_samples / elapsed:.1f} samples/sec")

    if seq_gaps > 0 or overflow > 0:
        print(f"  Pi-side polling can't keep up at 1000 Hz - this is expected")
        print(f"  the V3 kernel driver + IRQ-driven reads fixes this")
    else:
        print(f"  no loss even at 1000 Hz - MCU and Pi both kept up")

    stop_and_clear()
    print()

# ══════════════════════════════════════════════
#  Part 3.5: frequency sweep - find where problems start appearing
# ══════════════════════════════════════════════
def test_frequency_sweep(rates=(50, 100, 200, 300, 500, 700, 800, 900, 1000),
                          duration_per_rate=3.0):
    global echo_mismatch_count
    print("=" * 50)
    print("Frequency sweep: find the rate where echo errors start")
    print("=" * 50)

    results = []

    for rate in rates:
        stop_and_clear()
        echo_mismatch_count = 0

        write_reg(SAMPLE_RATE, rate)
        write_reg(CONTROL, 0x01)

        total_samples = 0
        seq_gaps = 0
        last_sequence = -1
        t0 = time.time()

        while time.time() - t0 < duration_per_rate:
            level = read_reg(FIFO_LEVEL)
            if level == 0:
                time.sleep(0.001)
                continue
            while level > 0 and time.time() - t0 < duration_per_rate:
                seq = read_reg(DATA_SEQ)
                val = read_reg(DATA_VAL)
                total_samples += 1
                if last_sequence >= 0:
                    expected = (last_sequence + 1) & 0xFFFFFFFF
                    if seq != expected:
                        seq_gaps += 1
                last_sequence = seq
                level = read_reg(FIFO_LEVEL)

        elapsed = time.time() - t0
        write_reg(CONTROL, 0x00)
        mismatches = echo_mismatch_count

        status = "OK" if mismatches == 0 else f"BAD ({mismatches} echo errors)"
        print(f"  {rate:5d} Hz | elapsed {elapsed:5.1f}s | samples={total_samples:5d} | gaps={seq_gaps:4d} | {status}")
        results.append((rate, mismatches, seq_gaps, elapsed))

    stop_and_clear()

    print("\n  --- summary ---")
    first_bad = None
    for rate, mismatches, seq_gaps, elapsed in results:
        if mismatches > 0 and first_bad is None:
            first_bad = rate
    if first_bad:
        print(f"  echo errors start appearing at {first_bad} Hz")
    else:
        print(f"  no echo errors at any tested rate")
    print()

# ══════════════════════════════════════════════
#  Part 4: 10000-frame protocol stress test (no acquisition)
# ══════════════════════════════════════════════
def test_stress():
    print("=" * 50)
    print("Part 4: 10000-frame protocol stress test")
    print("=" * 50)

    stop_and_clear()
    xfer(NOP)

    echo_mismatch = 0
    total = 10000

    t0 = time.time()
    for i in range(total):
        addr = random.choice([DEVICE_ID, FW_VERSION, STATUS, SAMPLE_RATE])
        xfer(addr)
        r = xfer(NOP)
        if r[0] != addr:
            echo_mismatch += 1

        if (i + 1) % 2000 == 0:
            print(f"  progress: {i+1}/{total}  mismatches: {echo_mismatch}")

    elapsed = time.time() - t0

    # check the SPI_RESYNC flag
    st = read_reg(STATUS)
    resync = "yes" if (st & 0x08) else "no"

    print(f"\n  --- results ---")
    print(f"  echo mismatches: {echo_mismatch}")
    print(f"  SPI_RESYNC:      {resync}")
    print(f"  elapsed:         {elapsed:.1f} sec")
    print(f"  frame rate:      {total / elapsed:.0f} frames/sec")

    if echo_mismatch == 0:
        print(f"  protocol layer stable")
    else:
        print(f"  mismatches present, needs investigation")

    print()

# ══════════════════════════════════════════════
#  Main
# ══════════════════════════════════════════════
if __name__ == "__main__":
    print("\n" + "=" * 50)
    print("  V1.3 full test suite")
    print("=" * 50 + "\n")

    test_regression()
    test_acquisition_slow()
    test_acquisition_fast()
    test_stress()
    #test_frequency_sweep()


    print("=" * 50)
    print("  all tests complete")
    print("=" * 50)

    spi.close()
