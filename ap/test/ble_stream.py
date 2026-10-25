#!/usr/bin/env python3
"""Read iotd's BLE advertisement stream and summarize it.

    python3 ble_stream.py <ap-address> [--port 6639] [--seconds 30] [--quiet]

Each line from the daemon is one JSON object. Prints every record unless --quiet,
then a per-address summary with counts and last RSSI.
"""
import argparse
import json
import socket
import sys
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("--port", type=int, default=6639)
    ap.add_argument("--seconds", type=float, default=30.0)
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    s = socket.create_connection((args.host, args.port), timeout=5)
    s.settimeout(1.0)
    deadline = time.monotonic() + args.seconds
    buf = b""
    total = 0
    per_addr = {}
    kinds = {"legacy": 0, "extended": 0, "scan_response": 0}
    while time.monotonic() < deadline:
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            continue
        if not chunk:
            print("server closed the connection", file=sys.stderr)
            break
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            try:
                rec = json.loads(line)
            except json.JSONDecodeError:
                print("bad line:", line, file=sys.stderr)
                continue
            total += 1
            a = rec["address"]
            ent = per_addr.setdefault(a, {"n": 0, "rssi": None, "type": rec["address_type"], "bytes": 0})
            ent["n"] += 1
            ent["rssi"] = rec["rssi"]
            ent["bytes"] = len(rec["data"]) // 2
            kinds["extended" if rec["extended"] else "legacy"] += 1
            if rec["scan_response"]:
                kinds["scan_response"] += 1
            if not args.quiet:
                print(line.decode())
    s.close()
    print(f"\n{total} records in {args.seconds:.0f} s from {len(per_addr)} addresses "
          f"({kinds['legacy']} legacy, {kinds['extended']} extended, {kinds['scan_response']} scan responses)")
    for a, e in sorted(per_addr.items(), key=lambda kv: -kv[1]["n"]):
        print(f"  {a}  type {e['type']}  {e['n']:5d} records  last rssi {e['rssi']:4d}  {e['bytes']:3d} AD bytes")


if __name__ == "__main__":
    main()
