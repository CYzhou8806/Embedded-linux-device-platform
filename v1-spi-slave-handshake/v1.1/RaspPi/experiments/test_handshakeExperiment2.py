#!/usr/bin/env python3
"""Experiment 2: How fast can we go before it breaks?"""
import spidev

spi = spidev.SpiDev()
spi.open(0, 0)
spi.mode = 0

speeds = [100_000, 500_000, 1_000_000, 2_000_000,
          4_000_000, 8_000_000, 16_000_000]

for speed in speeds:
    spi.max_speed_hz = speed
    # Run 10 times at each speed to check stability
    errors = 0
    for _ in range(10):
        rx = spi.xfer2([0x00, 0xFF, 0xFF, 0xFF, 0xFF])
        if rx != [0xA5, 0xAC, 0x00, 0xAC, 0xC0]:
            errors += 1
    
    actual = spi.max_speed_hz  # Pi might round to nearest supported
    status = f"{errors}/10 errors" if errors > 0 else "ALL OK"
    print(f"{speed/1e6:6.1f} MHz (actual ~{actual/1e6:.1f}): {status}")

spi.close()
