# ruckus-iot-dual-ncp

Open firmware and AP software that run **Zigbee and Bluetooth LE at the same time** on the
built-in IoT radio of Ruckus access points, for use with Home Assistant (ZHA), zigbee2mqtt or any
other EZSP host, without the vendor IoT gateway package.

| Component | What it is |
|---|---|
| `dual-ncp-gsdk4.4.0-efr32mg21.gbl` | Radio firmware: Silicon Labs EmberZNet 7.4.0.0 Zigbee NCP (EZSP v13) and Bluetooth 7.0.0 NCP (BGAPI), multiplexed over one UART with CPC |
| `cpcd` | Silicon Labs' CPC daemon 4.9.1, the UART multiplexer, built for the AP |
| `iotd` | The AP daemon: supervises `cpcd`, serves Zigbee as EZSP-over-ASH on TCP 6638 and Bluetooth advertisements as JSON on TCP 6639 |
| `ncpflash` | Flashes a GBL image onto the radio through its Gecko bootloader |
| `ezsp_probe` | Diagnostic: queries the Zigbee NCP directly through `cpcd` |

Home Assistant connects to the AP the same way it connects to a network-attached serial
coordinator: `socket://<ap-address>:6638`.

## Hardware

Ruckus APs with a built-in IoT radio carry one of several Silicon Labs radios. This build targets
the **EFR32MG21**, the radio in current models. It has been tested on the R560.

| Item | Notes |
|---|---|
| Radio | EFR32MG21 (Series 2). The EFR32MG12 (Series 1) used by some models and USB dongles needs its own build target; the EFR32BG21 is Bluetooth-only silicon and cannot run Zigbee |
| Bootloader | The radio's factory Gecko bootloader (UART XMODEM). It is not modified |
| UART | `/dev/ttyMSM1` on the models tested so far. Set `uart_device_file` in `cpcd.conf` and pass the device to `ncpflash` if your model differs |
| Reset and boot lines | `/proc/v54bsp/iot_rst` and `/proc/v54bsp/iot_rcv`, from the Ruckus board support package |
| AP firmware | Needs a root shell on the AP. Files live in `/writable/data`, which persists across reboots |

## Getting the binaries

Every release is built by GitHub Actions from this repository's pinned sources and tools. Each
file has a SHA-256 in `SHA256SUMS` and a signed build-provenance attestation:

```bash
gh attestation verify dual-ncp-gsdk4.4.0-efr32mg21.gbl --repo ms264556/ruckus-iot-dual-ncp
sha256sum -c SHA256SUMS
```

`ap-armv7.tar.gz` contains the AP programs and their configuration files.

## Building

Linux, x86_64 or aarch64, with about 15 GB free. Releases are built by GitHub Actions on Ubuntu
24.04 arm64. A build on another host architecture works the same way but produces a slightly
different firmware binary, because the compiler itself is a different build.

```bash
sudo apt install git git-lfs unzip curl cmake gcc-arm-linux-gnueabi file
curl -LsSf https://astral.sh/uv/install.sh | sh     # uv, for the Python tools (python3-venv also works)
git lfs install
git clone --recurse-submodules --shallow-submodules https://github.com/ms264556/ruckus-iot-dual-ncp.git
cd ruckus-iot-dual-ncp

scripts/setup-tools.sh       # pinned slc-cli, Java 21, Python pack, Arm GNU 12.2.rel1, pygbl -> .tools/
make -C ap test              # ASH codec unit test
scripts/build-firmware.sh    # -> out/firmware/dual-ncp-gsdk4.4.0-efr32mg21.gbl
scripts/build-ap.sh          # -> out/ap/
```

The Gecko SDK submodule is large (several GB with its Git LFS content). `setup-tools.sh` checks
every download against a pinned SHA-256. To use an `arm-none-eabi` toolchain that is already
installed instead of Arm GNU 12.2.rel1, set `ARM_GCC_DIR` (for example `ARM_GCC_DIR=/usr`) before
running it.

How the firmware is made: `scripts/build-firmware.sh` generates the GSDK 4.4.0 sample
`zigbee_ncp-ble_ncp-uart` for the `brd4180b` EFR32MG21 radio board, switches its low-frequency
clocks to the internal LFRCO (the AP radio modules have no 32.768 kHz crystal), turns CPC link
encryption off, builds it, and packages it as a GBL at 0x4000 above the bootloader.

## Installing on an AP

Copy the release files to the AP, for example with the AP's own `scp` client pulling from a host
you control:

| File | Destination |
|---|---|
| `cpcd`, `iotd`, `ncpflash`, `ezsp_probe`, `start.sh`, `cpcd.conf` | `/writable/data/bin/` |
| `iotd.conf.example` | `/writable/data/iotd.conf` |
| `dual-ncp-gsdk4.4.0-efr32mg21.gbl` | `/writable/data/` |

Then, in a root shell on the AP:

```sh
chmod 755 /writable/data/bin/*
# Stop the vendor IoT gateway. Its supervisor is suspended, not killed: init would respawn it,
# and it reflashes the vendor image onto the radio when it runs.
for p in $(ps | grep '[r]ksiot_gw_base' | awk '{print $1}'); do kill -STOP $p; done
killall -9 rksiot_gw rksiot_ble rksiot_zb iotd cpcd 2>/dev/null

# Flash the radio once (about a minute).
/writable/data/bin/ncpflash /writable/data/dual-ncp-gsdk4.4.0-efr32mg21.gbl

# Start the daemon.
/writable/data/bin/start.sh
```

`start.sh` repeats the vendor-gateway steps, so after a reboot running it again is enough; the
radio keeps its firmware. Nothing starts `iotd` at boot yet.

To check the radio without the daemon: `killall iotd`, start `cpcd -c /writable/data/bin/cpcd.conf &`,
then run `ezsp_probe`. It prints the EZSP version (13), the stack version (7.4.0.0) and the radio's
EUI64.

To return the AP to the vendor gateway: `killall iotd`, then `kill -CONT` the `rksiot_gw_base`
process. It reflashes its own radio image within a couple of minutes.

## Using it

* **Home Assistant ZHA**: add the Zigbee integration with radio type EZSP and the serial path
  `socket://<ap-address>:6638`.
* **zigbee2mqtt**: `adapter: ember`, `port: tcp://<ap-address>:6638`.
* **Bluetooth**: connect to TCP 6639 and read one JSON object per line per advertisement.

Interfaces, configuration and behaviour are described in [`ap/README.md`](ap/README.md).

## Licences

The scripts, `iotd`, `ncpflash`, `ezsp_probe` and documentation in this repository are MIT
licensed. The firmware image and `cpcd` are built from Silicon Labs software under the Silicon
Labs Master Software License Agreement, which allows binary distribution for use with Silicon Labs
devices. See [`NOTICE.md`](NOTICE.md).

This project is not affiliated with or endorsed by Ruckus (CommScope) or Silicon Labs.
