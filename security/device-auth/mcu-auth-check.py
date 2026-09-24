#!/usr/bin/env python3
"""Verifier for the MCU's device-authentication answers.

    mcu-auth-check.py <ssh-host> <key-file> [--rounds N]

Makes a fresh nonce, has the Raspberry Pi pass it to the MCU through the
driver's auth_challenge / auth_response, and checks the answer against
HMAC-SHA256(K, "acq-auth-v1" || DEVICE_ID || FW_VERSION || nonce)[:16].
Also checks that the answer does NOT verify under a wrong key and changes
with the nonce. Prints the MCU's cycle count for each MAC.
"""
import argparse, hashlib, hmac, os, subprocess, sys

D = "/sys/bus/spi/devices/spi0.0"

def on_pi(host, script):
    return subprocess.run(["ssh", "-o", "BatchMode=yes", host, script],
                          capture_output=True, text=True).stdout.split()

def expected(key, dev, fw, nonce):
    msg = b"acq-auth-v1" + dev.to_bytes(4, "big") + fw.to_bytes(4, "big") + nonce
    return hmac.new(key, msg, hashlib.sha256).digest()[:16].hex()

ap = argparse.ArgumentParser()
ap.add_argument("host"); ap.add_argument("key"); ap.add_argument("--rounds", type=int, default=5)
a = ap.parse_args()
key = open(a.key, "rb").read()
dev, fw = (int(x, 16) for x in on_pi(a.host, f"cat {D}/device_id {D}/fw_version"))
print(f"device_id=0x{dev:08x} fw_version=0x{fw:08x}")

nonces = [os.urandom(16) for _ in range(a.rounds)]
out = []
for i in range(0, len(nonces), 10):  # batches: one SSH command line per 10 rounds
    script = "; ".join(
        f'sudo sh -c "printf {n.hex()} > {D}/auth_challenge" && sudo cat {D}/auth_response && cat {D}/auth_cycles'
        for n in nonces[i:i + 10])
    out += on_pi(a.host, script)
if len(out) != 2 * a.rounds:
    sys.exit(f"unexpected output from the device ({len(out)} fields): {out[:6]}")

ok, macs, cycles = 0, [], []
for i, n in enumerate(nonces):
    got, cyc = out[2 * i], int(out[2 * i + 1])
    good = got == expected(key, dev, fw, n)
    wrong_key = got == expected(os.urandom(32), dev, fw, n)
    ok += good and not wrong_key
    macs.append(got); cycles.append(cyc)
    if a.rounds <= 10: print(f"nonce {n.hex()} -> {got}  {'VALID' if good else 'INVALID'}"
          f"  wrong-key check: {'accepted?!' if wrong_key else 'rejected'}  cycles={cyc}")
distinct = len(set(macs)) == len(macs)
print(f"{ok}/{a.rounds} valid, answers distinct per nonce: {distinct}")
print(f"cycles: min {min(cycles)} max {max(cycles)} -> {min(cycles)/72:.1f}..{max(cycles)/72:.1f} us at 72 MHz")
sys.exit(0 if ok == a.rounds and distinct else 1)
