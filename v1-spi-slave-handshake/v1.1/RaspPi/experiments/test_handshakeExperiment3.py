#!/usr/bin/env python3
"""Experiment 3: What if Pi sends fewer or more bytes than MCU expects?"""
import spidev

spi = spidev.SpiDev()
spi.open(0, 0)
spi.max_speed_hz = 500_000
spi.mode = 0

# MCU expects 5 bytes. What if we send fewer?
for length in [1, 3, 5, 7, 10]:
    tx = [0x00] + [0xFF] * (length - 1)
    rx = spi.xfer2(tx)
    print(f"Sent {length} bytes: {' '.join(f'{b:02X}' for b in rx)}")

spi.close()
