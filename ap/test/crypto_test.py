#!/usr/bin/env python3
"""Checks iotd's crypto against Python's cryptography package with random inputs.

    make -C ap test/crypto_vectors && python3 ap/test/crypto_test.py ap/test/crypto_vectors

Covers SHA-256 and HMAC-SHA256 across block boundaries, ChaCha20-Poly1305 seal and open (including a
tampered tag), and X25519 against both the base point and random points.
"""
import hashlib
import hmac
import os
import random
import subprocess
import sys

from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
from cryptography.hazmat.primitives import serialization

RAW = serialization.Encoding.Raw


def h(b):
    return b.hex() if b else "-"


def main():
    proc = subprocess.Popen([sys.argv[1]], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

    def ask(line):
        proc.stdin.write(line + "\n")
        proc.stdin.flush()
        return proc.stdout.readline().strip()

    failures = 0

    def check(what, got, want):
        nonlocal failures
        if got != want:
            failures += 1
            print(f"FAIL {what}: got {got} want {want}")

    rng = random.Random(1)
    lengths = [0, 1, 55, 56, 63, 64, 65, 119, 120, 128, 1000, 4096]
    for n in lengths + [rng.randrange(0, 3000) for _ in range(40)]:
        m = os.urandom(n)
        check(f"sha256 {n}", ask(f"sha256 {h(m)}"), hashlib.sha256(m).hexdigest())
        for kn in (0, 16, 32, 64, 65, 100):
            k = os.urandom(kn)
            check(f"hmac {kn}/{n}", ask(f"hmac {h(k)} {h(m)}"), hmac.new(k, m, hashlib.sha256).hexdigest())

    for n in [0, 1, 15, 16, 17, 63, 64, 65, 200, 1500, 65535 - 16] + [rng.randrange(0, 5000) for _ in range(40)]:
        key, nonce = os.urandom(32), os.urandom(12)
        ad = os.urandom(rng.choice([0, 0, 1, 16, 17, 32]))
        pt = os.urandom(n)
        want = ChaCha20Poly1305(key).encrypt(nonce, pt, ad or None)
        sealed = ask(f"seal {h(key)} {h(nonce)} {h(ad)} {h(pt)}")
        check(f"seal {n}/{len(ad)}", sealed, want.hex())
        check(f"open {n}", ask(f"open {h(key)} {h(nonce)} {h(ad)} {h(want)}"), h(pt))
        bad = bytearray(want)
        bad[-1] ^= 1
        check(f"open tampered {n}", ask(f"open {h(key)} {h(nonce)} {h(ad)} {h(bytes(bad))}"), "FAIL")

    for _ in range(60):
        priv = X25519PrivateKey.generate()
        scalar = priv.private_bytes(RAW, serialization.PrivateFormat.Raw, serialization.NoEncryption())
        pub = priv.public_key().public_bytes(RAW, serialization.PublicFormat.Raw)
        check("x25519 base", ask(f"x25519 {h(scalar)} -"), pub.hex())
        other = X25519PrivateKey.generate().public_key()
        shared = priv.exchange(other)
        check("x25519 dh", ask(f"x25519 {h(scalar)} {h(other.public_bytes(RAW, serialization.PublicFormat.Raw))}"), shared.hex())

    proc.stdin.close()
    proc.wait()
    print("crypto: all checks passed" if failures == 0 else f"crypto: {failures} failures")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
