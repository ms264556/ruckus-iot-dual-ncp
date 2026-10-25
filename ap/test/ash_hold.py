#!/usr/bin/env python3
"""Minimal ASH client for fault tests: connect to iotd's Zigbee port, send RST, print
the RSTACK, then hold the connection and report when and why it ends.

    python3 ash_hold.py <ap-address> [--port 6638] [--seconds 40]
"""
import argparse
import socket
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("--port", type=int, default=6638)
    ap.add_argument("--seconds", type=float, default=40.0)
    args = ap.parse_args()

    t0 = time.monotonic()
    s = socket.create_connection((args.host, args.port), timeout=5)
    s.sendall(bytes([0x1A] * 4) + bytes.fromhex("c038bc7e"))  # CANCEL x4, RST
    s.settimeout(1.0)
    buf = b""
    deadline = time.monotonic() + args.seconds
    while time.monotonic() < deadline:
        try:
            chunk = s.recv(4096)
        except socket.timeout:
            continue
        t = time.monotonic() - t0
        if not chunk:
            print(f"{t:6.2f}s server closed the connection")
            return
        buf += chunk
        print(f"{t:6.2f}s rx {chunk.hex()}")
        if buf.startswith(b"\xc1\x02"):
            print(f"{t:6.2f}s RSTACK version {buf[1]} reset code 0x{buf[2]:02x}")
            buf = b""
    print(f"{time.monotonic() - t0:6.2f}s still connected at the end of the test")
    s.close()


if __name__ == "__main__":
    main()
