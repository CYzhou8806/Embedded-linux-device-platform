#!/usr/bin/env python3
"""V1.1: Verify SPI slave handshake with STM32 acquisition controller."""

import spidev
import time

spi = spidev.SpiDev()
spi.open(0, 0)                # bus 0, CE0
spi.max_speed_hz = 500_000    # 500 kHz - slow and stable for first test
spi.mode = 0                  # CPOL=0, CPHA=0

# Send: cmd 0x00 (read DEVICE_ID) + 4 dummy bytes
# Expected reply: [0xA5, 0xAC, 0x00, 0xAC, 0xC0]
tx = [0x00, 0xFF, 0xFF, 0xFF, 0xFF]

print("Sending:  ", ' '.join(f'{b:02X}' for b in tx))
rx = spi.xfer2(tx)
print("Received: ", ' '.join(f'{b:02X}' for b in rx))

# Verify
EXPECTED_STATUS = 0xA5
EXPECTED_MAGIC = [0xAC, 0x00, 0xAC, 0xC0]

if rx[0] == EXPECTED_STATUS and rx[1:5] == EXPECTED_MAGIC:
    device_id = (rx[1] << 24) | (rx[2] << 16) | (rx[3] << 8) | rx[4]
    print(f"\n✅ HANDSHAKE OK")
    print(f"   Status byte: 0x{rx[0]:02X}")
    print(f"   DEVICE_ID:   0x{device_id:08X}")
else:
    print(f"\n❌ HANDSHAKE FAILED")
    print(f"   Expected: A5 AC 00 AC C0")
    print(f"   Got:      {' '.join(f'{b:02X}' for b in rx)}")

spi.close()
