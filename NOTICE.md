# Notices

## This repository

Everything committed to this repository (build scripts, `ap/`, `firmware/*.py`, documentation)
is licensed under the MIT licence in [`LICENSE`](LICENSE).

## Silicon Labs software

Two Silicon Labs projects are referenced as Git submodules and are not part of this repository:

| Submodule | Upstream | Version |
|---|---|---|
| `third_party/gecko_sdk` | https://github.com/SiliconLabs/gecko_sdk | v4.4.0 |
| `third_party/cpc-daemon` | https://github.com/SiliconLabs/cpc-daemon | v4.9.1 |

Both are distributed by Silicon Labs under the
[Silicon Labs Master Software License Agreement (MSLA)](https://www.silabs.com/about-us/legal/master-software-license-agreement),
with third-party components under the licences listed in each project. Fetching the submodules
means accepting those terms.

The build outputs contain that software:

* the radio firmware image (`*.gbl`) contains GSDK components, including precompiled Zigbee and
  Bluetooth stack libraries;
* `cpcd`, `iotd` and `ezsp_probe` contain cpc-daemon code (`cpcd` itself, and the statically
  linked `libcpc` client library).

The MSLA permits distributing these in binary form for use with Silicon Labs devices; the AP's
IoT radio is such a device. Silicon Labs' copyright and licence notices that the SDK embeds in
built images are retained.

## GNU C Library

The AP programs (`cpcd`, `iotd`, `ncpflash`, `ezsp_probe`) are statically linked against the GNU
C Library from the Ubuntu `gcc-arm-linux-gnueabi` cross toolchain, which is licensed under the GNU
Lesser General Public License v2.1 or later. The complete source and build scripts for these
programs are in this repository and its submodules, so they can be rebuilt and relinked against a
modified library with `scripts/build-ap.sh`.

## Build tools

The build tools downloaded by `scripts/setup-tools.sh` (slc-cli, the Silicon Labs Python and Java
packs, the Arm GNU Toolchain) are not redistributed and keep their own licences.

## Ruckus

This repository contains no Ruckus software. The AP's own firmware, and the vendor IoT gateway
package that this project replaces, remain Ruckus's (CommScope's) property and are neither copied
nor redistributed here.

## No warranty

Flashing an AP's radio is at your own risk. The radio's Gecko bootloader is not modified, and
the vendor gateway restores its own image when it is allowed to run again.
