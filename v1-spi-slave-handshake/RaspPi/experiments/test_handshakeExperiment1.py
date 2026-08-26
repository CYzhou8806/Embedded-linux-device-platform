#!/usr/bin/env python3
"""Experiment 1: What happens when SPI mode is wrong?"""
import spidev

spi = spidev.SpiDev()
spi.open(0, 0)
spi.max_speed_hz = 500_000

for mode in [0, 1, 2, 3]:
    spi.mode = mode
    tx = [0x00, 0xFF, 0xFF, 0xFF, 0xFF]
    rx = spi.xfer2(tx)
    status = "OK" if rx == [0xA5, 0xAC, 0x00, 0xAC, 0xC0] else "WRONG"
    print(f"Mode {mode}: {' '.join(f'{b:02X}' for b in rx)}  [{status}]")

spi.close()
