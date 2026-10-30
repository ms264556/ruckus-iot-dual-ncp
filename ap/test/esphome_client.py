#!/usr/bin/env python3
"""A minimal ESPHome native API client with Noise encryption, for testing iotd's Bluetooth proxy.

    python3 esphome_client.py <host> --key <base64> [--port 6053] [--seconds 10]
    python3 esphome_client.py <host> --key <wrong base64> --expect-reject
    python3 esphome_client.py <host> --plaintext            # expects the "Bad indicator byte" reject

Runs the client side the way aioesphomeapi does (Noise_NNpsk0_25519_ChaChaPoly_SHA256, prologue
"NoiseAPIInit\\0\\0"): hello, handshake, HelloRequest, ConnectRequest, DeviceInfoRequest, then subscribes to
raw Bluetooth advertisements and prints a summary. Independent of iotd's own code: Python's cryptography
package does the primitives. Exits non-zero when something is wrong.
"""
import argparse
import base64
import hashlib
import hmac
import socket
import struct
import sys
import time

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305

RAW = serialization.Encoding.Raw


def hkdf(ck, ikm, n):
    temp = hmac.new(ck, ikm, hashlib.sha256).digest()
    o1 = hmac.new(temp, b"\x01", hashlib.sha256).digest()
    o2 = hmac.new(temp, o1 + b"\x02", hashlib.sha256).digest()
    o3 = hmac.new(temp, o2 + b"\x03", hashlib.sha256).digest() if n == 3 else None
    return o1, o2, o3


class Cipher:
    def __init__(self, key):
        self.aead, self.n = ChaCha20Poly1305(key), 0

    def nonce(self):
        return b"\x00" * 4 + struct.pack("<Q", self.n)

    def encrypt(self, ad, pt):
        ct = self.aead.encrypt(self.nonce(), pt, ad)
        self.n += 1
        return ct

    def decrypt(self, ad, ct):
        pt = self.aead.decrypt(self.nonce(), ct, ad)
        self.n += 1
        return pt


class Initiator:
    def __init__(self, psk):
        self.psk = psk
        self.h = hashlib.sha256(b"Noise_NNpsk0_25519_ChaChaPoly_SHA256").digest()
        self.ck, self.c = self.h, None
        self.e = X25519PrivateKey.generate()
        self.mix_hash(b"NoiseAPIInit\x00\x00")

    def mix_hash(self, d):
        self.h = hashlib.sha256(self.h + d).digest()

    def mix_key(self, ikm):
        self.ck, k, _ = hkdf(self.ck, ikm, 2)
        self.c = Cipher(k)

    def write1(self):
        self.ck, th, k = hkdf(self.ck, self.psk, 3)
        self.mix_hash(th)
        self.c = Cipher(k)
        e = self.e.public_key().public_bytes(RAW, serialization.PublicFormat.Raw)
        self.mix_hash(e)
        self.mix_key(e)
        ct = self.c.encrypt(self.h, b"")
        self.mix_hash(ct)
        return e + ct

    def read2(self, m):
        re = m[:32]
        self.mix_hash(re)
        self.mix_key(re)
        self.mix_key(self.e.exchange(X25519PublicKey.from_public_bytes(re)))
        self.c.decrypt(self.h, m[32:])
        self.mix_hash(m[32:])
        k1, k2, _ = hkdf(self.ck, b"", 2)
        return Cipher(k1), Cipher(k2)  # send, receive


def varint(v):
    out = bytearray()
    while v >= 0x80:
        out.append((v & 0x7F) | 0x80)
        v >>= 7
    out.append(v)
    return bytes(out)


def pb(*fields):
    out = bytearray()
    for num, val in fields:
        if isinstance(val, (bytes, str)):
            val = val.encode() if isinstance(val, str) else val
            out += varint(num << 3 | 2) + varint(len(val)) + val
        else:
            out += varint(num << 3) + varint(val)
    return bytes(out)


def parse(buf):
    """field -> list of values (ints or bytes)."""
    fields, i = {}, 0
    while i < len(buf):
        tag = 0
        for s in range(0, 64, 7):
            b = buf[i]; i += 1
            tag |= (b & 0x7F) << s
            if not b & 0x80:
                break
        num, wire = tag >> 3, tag & 7
        if wire == 0:
            v = 0
            for s in range(0, 64, 7):
                b = buf[i]; i += 1
                v |= (b & 0x7F) << s
                if not b & 0x80:
                    break
        elif wire == 2:
            n = 0
            for s in range(0, 64, 7):
                b = buf[i]; i += 1
                n |= (b & 0x7F) << s
                if not b & 0x80:
                    break
            v = buf[i:i + n]; i += n
        elif wire == 5:
            v = struct.unpack("<I", buf[i:i + 4])[0]; i += 4
        else:
            raise ValueError(f"wire type {wire}")
        fields.setdefault(num, []).append(v)
    return fields


class Conn:
    def __init__(self, host, port):
        self.s = socket.create_connection((host, port), timeout=10)
        self.buf = b""

    def send_frame(self, payload):
        self.s.sendall(b"\x01" + struct.pack(">H", len(payload)) + payload)

    def read_frame(self, timeout=10):
        self.s.settimeout(timeout)
        while True:
            if len(self.buf) >= 3:
                if self.buf[0] != 1:
                    raise ValueError(f"bad indicator {self.buf[0]}")
                n = struct.unpack(">H", self.buf[1:3])[0]
                if len(self.buf) >= 3 + n:
                    f, self.buf = self.buf[3:3 + n], self.buf[3 + n:]
                    return f
            chunk = self.s.recv(65536)
            if not chunk:
                raise EOFError("closed by the server")
            self.buf += chunk


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("--port", type=int, default=6053)
    ap.add_argument("--key")
    ap.add_argument("--seconds", type=float, default=10)
    ap.add_argument("--expect-reject", action="store_true")
    ap.add_argument("--plaintext", action="store_true")
    args = ap.parse_args()

    if args.plaintext:
        s = socket.create_connection((args.host, args.port), timeout=10)
        s.sendall(b"\x00\x05\x01" + pb((1, "plaintext")))  # plaintext HelloRequest
        reply = s.recv(1024)
        ok = reply[:1] == b"\x01" and b"Bad indicator byte" in reply
        print(f"plaintext client: {'rejected with Bad indicator byte' if ok else f'unexpected reply {reply!r}'}")
        return 0 if ok else 1

    psk = base64.b64decode(args.key)
    c = Conn(args.host, args.port)
    noise = Initiator(psk)
    c.send_frame(b"")                       # client hello, as aioesphomeapi sends it
    c.send_frame(b"\x00" + noise.write1())  # handshake message 1 right after
    hello = c.read_frame()
    name, mac = hello[1:].split(b"\x00")[:2]
    print(f"server hello: protocol {hello[0]}, name {name.decode()}, mac {mac.decode()}")
    reply = c.read_frame()
    if reply[0] != 0:
        print(f"handshake rejected: {reply[1:].decode(errors='replace')}")
        return 0 if args.expect_reject and b"Handshake MAC failure" in reply else 1
    if args.expect_reject:
        print("handshake accepted, but a reject was expected")
        return 1
    send, recv = noise.read2(reply[1:])
    print("encrypted session established")

    def request(mtype, msg=b""):
        c.send_frame(send.encrypt(b"", struct.pack(">HH", mtype, len(msg)) + msg))

    def receive(timeout=10):
        p = recv.decrypt(b"", c.read_frame(timeout))
        mtype, n = struct.unpack(">HH", p[:4])
        return mtype, p[4:4 + n]

    request(1, pb((1, "esphome_client.py"), (2, 1), (3, 10)))
    t, m = receive()
    f = parse(m)
    print(f"HelloResponse: API {f.get(1, [0])[0]}.{f.get(2, [0])[0]}, server {f.get(3, [b''])[0].decode()}, name {f.get(4, [b''])[0].decode()}")
    assert t == 2
    request(3)
    t, _ = receive()
    assert t == 4
    request(9)
    t, m = receive()
    assert t == 10
    f = parse(m)
    names = {2: "name", 3: "mac_address", 4: "esphome_version", 6: "model", 12: "manufacturer", 13: "friendly_name",
             15: "bluetooth_proxy_feature_flags", 18: "bluetooth_mac_address", 19: "api_encryption_supported"}
    for num, label in names.items():
        if num in f:
            v = f[num][0]
            print(f"  {label}: {v.decode() if isinstance(v, bytes) else v}")
    request(36)
    t, m = receive()
    print(f"GetTimeResponse: {time.strftime('%Y-%m-%d %H:%M:%S', time.gmtime(parse(m)[1][0]))} UTC")
    request(66, pb((1, 1)))                   # subscribe, raw advertisements
    adverts, batches, addresses = 0, 0, set()
    deadline = time.monotonic() + args.seconds
    while time.monotonic() < deadline:
        try:
            t, m = receive(timeout=max(0.1, deadline - time.monotonic()))
        except socket.timeout:
            break
        if t == 93:
            batches += 1
            for raw in parse(m).get(1, []):
                a = parse(raw)
                adverts += 1
                addresses.add(a[1][0])
        elif t == 7:
            request(8)
    request(7)
    while True:
        t, _ = receive()
        if t == 8:
            break
    request(87)                               # unsubscribe; batches already sent still arrive
    request(5)
    while True:
        try:
            t, _ = receive(timeout=5)
        except (socket.timeout, EOFError):
            t = None
            break
        if t == 6:
            break
    print(f"adverts: {adverts} in {batches} batches from {len(addresses)} addresses over {args.seconds:.0f} s; ping ok; disconnect {'ok' if t == 6 else t}")
    return 0 if t == 6 else 1


if __name__ == "__main__":
    sys.exit(main())
