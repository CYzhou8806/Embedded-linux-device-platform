#!/usr/bin/env python3
"""Check Core/Src/devauth.c against Python's hashlib/hmac.

Builds the C file for the host, then compares SHA-256 and HMAC-SHA256 on
the RFC 4231 test vectors and on random inputs of every length around the
SHA-256 block and padding boundaries, and the device-auth MAC format."""
import hashlib, hmac, os, subprocess, sys, tempfile

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.dirname(here)
exe = os.path.join(tempfile.mkdtemp(), "devauth_test")
subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-I", f"{root}/Core/Inc",
                f"{root}/Core/Src/devauth.c", f"{here}/devauth_test.c", "-o", exe], check=True)

cases, expect = [], []
def h(b): return b.hex() if b else "-"

# RFC 4231 test cases 1, 2, 3, 6, 7 (6 and 7 use a 131-byte key > block size)
rfc = [(b"\x0b" * 20, b"Hi There"),
       (b"Jefe", b"what do ya want for nothing?"),
       (b"\xaa" * 20, b"\xdd" * 50),
       (b"\xaa" * 131, b"Test Using Larger Than Block-Size Key - Hash Key First"),
       (b"\xaa" * 131, b"This is a test using a larger than block-size key and a larger than "
                       b"block-size data. The key needs to be hashed before being used by the HMAC algorithm.")]
for k, m in rfc:
    cases.append(f"hmac {h(k)} {h(m)}"); expect.append(hmac.new(k, m, hashlib.sha256).hexdigest())
for n in list(range(0, 130)) + [255, 256, 1000]:
    m = os.urandom(n)
    cases.append(f"sha {h(m)}"); expect.append(hashlib.sha256(m).hexdigest())
    k = os.urandom(32)
    cases.append(f"hmac {h(k)} {h(m)}"); expect.append(hmac.new(k, m, hashlib.sha256).hexdigest())
for _ in range(50):
    k, nonce = os.urandom(32), os.urandom(16)
    dev, fw = int.from_bytes(os.urandom(4), "big"), int.from_bytes(os.urandom(4), "big")
    msg = b"acq-auth-v1" + dev.to_bytes(4, "big") + fw.to_bytes(4, "big") + nonce
    cases.append(f"auth {k.hex()} {dev:08x} {fw:08x} {nonce.hex()}")
    expect.append(hmac.new(k, msg, hashlib.sha256).hexdigest()[:32])

out = subprocess.run([exe], input="\n".join(cases) + "\n", capture_output=True, text=True).stdout.split()
bad = [(c, e, o) for c, e, o in zip(cases, expect, out) if e != o]
print(f"{len(cases)} cases, {len(bad)} mismatches" + ("" if len(out) == len(cases) else f" (got {len(out)} outputs)"))
for c, e, o in bad[:5]:
    print(" ", c[:60], "\n    expected", e, "\n    got     ", o)
sys.exit(1 if bad or len(out) != len(cases) else 0)
