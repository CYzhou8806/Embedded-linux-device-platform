#!/usr/bin/env python3
"""Experiment 4: Rapid repeated transactions - is MCU fast enough to re-enter?"""
import spidev
import time

spi = spidev.SpiDev()
spi.open(0, 0)
spi.max_speed_hz = 500_000
spi.mode = 0

errors = 0
total = 1000

start = time.time()
for i in range(total):
    rx = spi.xfer2([0x00, 0xFF, 0xFF, 0xFF, 0xFF])
    if rx != [0xA5, 0xAC, 0x00, 0xAC, 0xC0]:
        errors += 1
        if errors <= 5:  # Only print first 5 errors
            print(f"  Error at #{i}: {' '.join(f'{b:02X}' for b in rx)}")

elapsed = time.time() - start
print(f"\n{total} transactions in {elapsed:.2f}s")
print(f"Rate: {total/elapsed:.0f} transactions/sec")
print(f"Errors: {errors}/{total}")

spi.close()

