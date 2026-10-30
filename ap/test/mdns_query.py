#!/usr/bin/env python3
"""Asks the LAN for ESPHome devices over mDNS and prints what answers, as Home Assistant's discovery sees them.

    python3 mdns_query.py [--seconds 3] [--unicast]

Sends one PTR query for _esphomelib._tcp.local (multicast, from port 5353, or with --unicast from an
ephemeral port, which makes responders answer the legacy unicast way) and prints every instance with its
SRV target and port, TXT entries and A addresses found in the answers and additionals.
"""
import argparse
import socket
import struct
import time


def name_at(buf, off):
    labels, jumped, end = [], False, off
    while True:
        n = buf[off]
        if n == 0:
            off += 1
            break
        if n & 0xC0 == 0xC0:
            if not jumped:
                end = off + 2
            off = ((n & 0x3F) << 8) | buf[off + 1]
            jumped = True
            continue
        labels.append(buf[off + 1:off + 1 + n].decode(errors="replace"))
        off += 1 + n
    return ".".join(labels), (end if jumped else off)


def query(service):
    q = struct.pack(">HHHHHH", 0, 0, 1, 0, 0, 0)
    for label in service.split("."):
        q += bytes([len(label)]) + label.encode()
    return q + b"\x00" + struct.pack(">HH", 12, 1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=3)
    ap.add_argument("--unicast", action="store_true")
    args = ap.parse_args()
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if hasattr(socket, "SO_REUSEPORT"):
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    s.bind(("", 0 if args.unicast else 5353))
    if not args.unicast:
        mreq = socket.inet_aton("224.0.0.251") + socket.inet_aton("0.0.0.0")
        s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
    s.sendto(query("_esphomelib._tcp.local"), ("224.0.0.251", 5353))
    s.settimeout(0.3)
    found = {}
    deadline = time.monotonic() + args.seconds
    while time.monotonic() < deadline:
        try:
            buf, src = s.recvfrom(9000)
        except socket.timeout:
            continue
        _, flags, qd, an, ns, ar = struct.unpack(">HHHHHH", buf[:12])
        if not flags & 0x8000:
            continue
        off = 12
        for _ in range(qd):
            _, off = name_at(buf, off)
            off += 4
        for _ in range(an + ns + ar):
            name, off = name_at(buf, off)
            rtype, _, ttl, rdlen = struct.unpack(">HHIH", buf[off:off + 10])
            rd = off + 10
            off = rd + rdlen
            entry = found.setdefault(src[0], {})
            if rtype == 12:
                entry.setdefault("ptr", set()).add(name_at(buf, rd)[0])
            elif rtype == 33:
                prio, weight, port = struct.unpack(">HHH", buf[rd:rd + 6])
                entry["srv"] = (name, name_at(buf, rd + 6)[0], port, ttl)
            elif rtype == 16:
                txt, i = [], rd
                while i < rd + rdlen:
                    n = buf[i]
                    txt.append(buf[i + 1:i + 1 + n].decode(errors="replace"))
                    i += 1 + n
                entry["txt"] = txt
            elif rtype == 1:
                entry.setdefault("a", set()).add((name, socket.inet_ntoa(buf[rd:rd + 4])))
    for src, e in found.items():
        print(f"from {src}:")
        for k, v in e.items():
            print(f"  {k}: {sorted(v) if isinstance(v, set) else v}")
    if not found:
        print("no answers")


if __name__ == "__main__":
    main()
