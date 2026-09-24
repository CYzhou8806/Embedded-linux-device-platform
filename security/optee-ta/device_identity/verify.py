#!/usr/bin/env python3
"""Verifier side of the device identity challenge.

Runs anywhere except on the device: it only needs the public key recorded at
provisioning. It makes the nonce, so a signature can't be replayed from an
earlier challenge.

    verify.py nonce                        -> print a fresh 32-byte nonce
    verify.py check <pubkey> <nonce> <sig> -> exit 0 if the signature is valid
"""
import hashlib
import os
import sys

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import (
    Prehashed, encode_dss_signature)

DOMAIN = b"device-platform/devid-challenge/v1"  # ta/include/device_identity_ta.h


def check(pub_hex: str, nonce_hex: str, sig_hex: str) -> bool:
    pub = ec.EllipticCurvePublicKey.from_encoded_point(
        ec.SECP256R1(), bytes.fromhex(pub_hex))
    sig = bytes.fromhex(sig_hex)
    if len(sig) != 64:
        return False
    der = encode_dss_signature(int.from_bytes(sig[:32], "big"),
                               int.from_bytes(sig[32:], "big"))
    digest = hashlib.sha256(DOMAIN + bytes.fromhex(nonce_hex)).digest()
    try:
        pub.verify(der, digest, ec.ECDSA(Prehashed(hashes.SHA256())))
        return True
    except InvalidSignature:
        return False


if __name__ == "__main__":
    if sys.argv[1:2] == ["nonce"]:
        print(os.urandom(32).hex())
    elif sys.argv[1:2] == ["check"] and len(sys.argv) == 5:
        ok = check(*sys.argv[2:5])
        print("VALID" if ok else "INVALID")
        sys.exit(0 if ok else 1)
    else:
        sys.exit(__doc__)
