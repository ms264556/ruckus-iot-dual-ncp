#!/usr/bin/env python3
"""Package a Gecko SDK build as a GBL3 application image at flash 0x4000.

The application must start at 0x4000, above the Gecko bootloader. The GBL is built
explicitly from the linked image (Intel hex or raw binary) rather than from the ELF,
whose load segments would place the program at 0x0:

    header (GBL3, spec version 3)
    ApplicationInfo (type, version 1, capabilities 0, zero product id)
    EraseProg at 0x4000 with the application data
    End (CRC)

Set OBJCOPY (default arm-none-eabi-objcopy) for .hex input.

Usage: make_gbl.py <app.bin|app.hex> <out.gbl> [--type bluetooth_app|zigbee|thread] [--version N]
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

APP_ADDRESS = 0x4000
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


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("binary", type=pathlib.Path, help="linked application (.bin or .hex)")
    parser.add_argument("output", type=pathlib.Path, help="GBL to write")
    parser.add_argument("--type", default="bluetooth_app", choices=sorted(TYPES))
    parser.add_argument("--version", type=int, default=1)
    parser.add_argument("--address", type=lambda v: int(v, 0), default=APP_ADDRESS)
    args = parser.parse_args(argv)

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
