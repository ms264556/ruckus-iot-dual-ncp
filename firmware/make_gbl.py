#!/usr/bin/env python3
"""Package a Gecko SDK build as a GBL3 application image at its link address.

The EraseProg address is where the application is linked: 0x4000 on Series 2 parts
(EFR32MG21, above the Gecko bootloader), 0x0 on Series 1 parts (EFR32MG12, whose
bootloader lives in its own flash region). For .hex input it is the lowest address in
the hex file; --address must agree with it when given. A raw .bin needs --address. The
GBL is built explicitly from the linked image rather than from the ELF:

    header (GBL3, spec version 3)
    ApplicationInfo (type, version 1, capabilities 0, zero product id)
    EraseProg at 0x4000 with the application data
    End (CRC)

Set OBJCOPY (default arm-none-eabi-objcopy) for .hex input.

Usage: make_gbl.py <app.bin|app.hex> <out.gbl> [--type bluetooth_app|zigbee|thread] [--version N] [--address A]
"""

from __future__ import annotations

import argparse
import os
import pathlib
import subprocess
import sys
import tempfile

try:
    from pygbl import (
        GBL3ApplicationInfo,
        GBL3ApplicationType,
        GBL3EraseProg,
        GBL3Header,
        GBL3Image,
        GBL3Type,
        parse_firmware_image,
    )
except ImportError:  # pragma: no cover
    sys.exit("pygbl is required: pip install 'pygbl[all]'")

GBL3_SPEC_VERSION = 3

TYPES = {
    "bluetooth_app": GBL3ApplicationType.BLUETOOTH_APP,
    "bluetooth": GBL3ApplicationType.BLUETOOTH,
    "zigbee": GBL3ApplicationType.ZIGBEE,
    "thread": GBL3ApplicationType.THREAD,
    "mcu": GBL3ApplicationType.MCU,
}


def load_binary(path: pathlib.Path) -> bytes:
    if path.suffix.lower() == ".hex":
        with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as tmp:
            bin_path = pathlib.Path(tmp.name)
        try:
            subprocess.run(
                [os.environ.get("OBJCOPY", "arm-none-eabi-objcopy"), "-I", "ihex", "-O", "binary", str(path), str(bin_path)],
                check=True,
            )
            return bin_path.read_bytes()
        finally:
            bin_path.unlink(missing_ok=True)
    return path.read_bytes()


def hex_base_address(path: pathlib.Path) -> int:
    """The lowest data address in an Intel hex file (records 00, with 02/04 segment and linear bases)."""
    base = 0
    lowest = None
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line.startswith(":"):
            continue
        record = bytes.fromhex(line[1:])
        length, address, kind = record[0], int.from_bytes(record[1:3], "big"), record[3]
        data = record[4:4 + length]
        if kind == 0x00 and length:
            start = base + address
            lowest = start if lowest is None else min(lowest, start)
        elif kind == 0x02:
            base = int.from_bytes(data, "big") << 4
        elif kind == 0x04:
            base = int.from_bytes(data, "big") << 16
        elif kind == 0x01:
            break
    if lowest is None:
        raise SystemExit(f"{path}: no data records")
    return lowest


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("binary", type=pathlib.Path, help="linked application (.bin or .hex)")
    parser.add_argument("output", type=pathlib.Path, help="GBL to write")
    parser.add_argument("--type", default="bluetooth_app", choices=sorted(TYPES))
    parser.add_argument("--version", type=int, default=1)
    parser.add_argument("--address", type=lambda v: int(v, 0), default=None,
                        help="link address of the application (taken from a .hex; required for a .bin)")
    args = parser.parse_args(argv)

    if args.binary.suffix.lower() == ".hex":
        linked = hex_base_address(args.binary)
        if args.address is not None and args.address != linked:
            print(f"error: --address 0x{args.address:X} but the hex starts at 0x{linked:X}", file=sys.stderr)
            return 1
        args.address = linked
    elif args.address is None:
        print("error: --address is required for a raw binary", file=sys.stderr)
        return 1

    data = load_binary(args.binary)
    image = GBL3Image(
        tags=[
            GBL3Header(version=GBL3_SPEC_VERSION, type=GBL3Type.NONE),
            GBL3ApplicationInfo(
                type=TYPES[args.type], version=args.version, capabilities=0, product_id=b"\x00" * 16
            ),
            GBL3EraseProg(address=args.address, data=data),
        ]
    ).regenerate_crc()

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image.serialize(block_size=4))

    parsed = parse_firmware_image(args.output.read_bytes())
    print(f"wrote {args.output} ({args.output.stat().st_size:,} bytes, app {len(data):,} bytes)")
    for tag in parsed.tags:
        name = type(tag).__name__
        if name == "GBL3EraseProg":
            print(f"  EraseProg addr=0x{tag.address:08X} len={len(tag.data):,}")
            if tag.address != args.address:
                print("  error: EraseProg address unexpected", file=sys.stderr)
                return 1
        elif name == "GBL3ApplicationInfo":
            print(f"  ApplicationInfo type={tag.type} version={tag.version}")
        else:
            print(f"  {name}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
