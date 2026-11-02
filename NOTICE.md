# Notices

## This repository

Everything committed to this repository (build scripts, `ap/`, `firmware/*.py`, `otbr/*.patch`,
documentation) is licensed under the MIT licence in [`LICENSE`](LICENSE).

## Licence texts

[`licenses/`](licenses) holds the licence and notice files of the third-party software the release
files contain, copied unchanged from the submodules, the pinned mDNSResponder tarball and the build
host:

| File | Copied from |
|---|---|
| `OpenThread-LICENSE.txt`, `OpenThread-NOTICE.txt` | `third_party/gecko_sdk/util/third_party/openthread/LICENSE`, `NOTICE` |
| `ot-br-posix-LICENSE.txt`, `ot-br-posix-NOTICE.txt` | `third_party/gecko_sdk/util/third_party/ot-br-posix/LICENSE`, `NOTICE` |
| `mDNSResponder-LICENSE.txt` | `LICENSE` in `mDNSResponder-1790.80.10.tar.gz` |
| `mDNSResponder-dns_sd-BSD-3-Clause.txt` | the licence header of `mDNSShared/dns_sd.h` in the same tarball (the client library has no separate licence file) |
| `Apache-2.0.txt` | `third_party/gecko_sdk/util/third_party/mbedtls/LICENSE` (identical to Mbed TLS 2.28.0's, and the Apache License 2.0 that mDNSResponder's `LICENSE` refers to) |
| `cJSON-LICENSE.txt` | `third_party/gecko_sdk/util/third_party/ot-br-posix/third_party/cJSON/repo/LICENSE` |
| `http-parser-LICENSE-MIT.txt` | `third_party/gecko_sdk/util/third_party/ot-br-posix/third_party/http-parser/repo/LICENSE-MIT` |
| `printf-LICENSE.txt` | the licence header of `third_party/gecko_sdk/util/third_party/printf/printf.c` |
| `gecko_sdk-License.txt` | `third_party/gecko_sdk/License.txt` (which GSDK files are MSLA and which Zlib) |
| `cpc-daemon-LICENSE.txt` | `third_party/cpc-daemon/LICENSE` |
| `LGPL-2.1.txt`, `GPL-3.0.txt` | `/usr/share/common-licenses/` on the Ubuntu build host |
| `GCC-Runtime-Library-Exception-3.1.txt` | the exception's text in `/usr/share/doc/gcc-*-base/copyright` on the Ubuntu build host |

`scripts/package.sh` puts `NOTICE.md`, `LICENSE` and every file in `licenses/` into `dist/`, so each
release carries them next to the binaries.

## What the release files contain

| Release file | Built from | Licences |
|---|---|---|
| `dual-ncp-gsdk4.4.0-efr32mg21.gbl`, `dual-ncp-gsdk4.4.0-efr32mg12.gbl` | GSDK 4.4.0 Zigbee NCP + Bluetooth NCP (`scripts/build-firmware.sh`) | Silicon Labs MSLA (Zigbee, Bluetooth host, NVM3 and other GSDK libraries and sources), Zlib (RAIL, Bluetooth link layer, most GSDK platform sources), Apache-2.0 (Mbed TLS 3.5.0, a CMSIS-RTOS2 source), MIT (printf), newlib-nano and libgcc from the Arm GNU Toolchain |
| `ot-rcp-gsdk4.4.0-efr32mg21.gbl`, `ot-rcp-gsdk4.4.0-efr32mg12.gbl` | GSDK 4.4.0 OpenThread RCP sample (`scripts/build-thread.sh`) | BSD-3-Clause (OpenThread, GSDK's OpenThread platform layer), Silicon Labs MSLA (`libsl_openthread_*`, NVM3, a few GSDK sources), Zlib (RAIL, most GSDK platform sources), Apache-2.0 (Mbed TLS 3.5.0), newlib-nano and libgcc from the Arm GNU Toolchain |
| `otbr-agent` | ot-br-posix and OpenThread from GSDK 4.4.0 (`scripts/build-otbr.sh`) | BSD-3-Clause (ot-br-posix, OpenThread), BSD-3-Clause (mDNSResponder `dns_sd` client), MIT (cJSON, http-parser), Apache-2.0 (Mbed TLS 2.28.0), Silicon Labs MSLA (`libcpc`), LGPL-2.1-or-later (glibc), GPL-3.0 with the GCC Runtime Library Exception (libstdc++, libgcc) |
| `ot-ctl` | OpenThread from GSDK 4.4.0 (`scripts/build-otbr.sh`) | BSD-3-Clause (OpenThread), LGPL-2.1-or-later (glibc), GPL-3.0 with the GCC Runtime Library Exception (libgcc) |
| `mdnsd` | mDNSResponder 1790.80.10 (`scripts/build-otbr.sh`) | Apache-2.0 (daemon), BSD-3-Clause (`dnssd_ipc.c`), LGPL-2.1-or-later (glibc), GPL-3.0 with the GCC Runtime Library Exception (libgcc) |
| `cpcd` | cpc-daemon v4.9.1 (`scripts/build-ap.sh`) | Silicon Labs MSLA, LGPL-2.1-or-later (glibc), GPL-3.0 with the GCC Runtime Library Exception (libgcc) |
| `iotd`, `ezsp_probe` | this repository, with cpc-daemon's `libcpc` (`scripts/build-ap.sh`) | MIT (this repository), Silicon Labs MSLA (`libcpc`), LGPL-2.1-or-later (glibc), GPL-3.0 with the GCC Runtime Library Exception (libgcc) |
| `ncpflash`, `spinel_probe` | this repository (`ap/tools`, `scripts/build-ap.sh`) | MIT (this repository), LGPL-2.1-or-later (glibc), GPL-3.0 with the GCC Runtime Library Exception (libgcc) |
| `start.sh`, `cpcd.conf`, `iotd.conf.example` | this repository | MIT |
| `ap-armv7.tar.gz` | the AP files above (`scripts/package.sh`) | as for each file it holds |

## Public-domain code that `ap/iotd/crypto.c` follows

`crypto.c` (the ESPHome API's Noise transport) is written for this repository, but two parts follow
public-domain implementations closely:

* X25519: the field arithmetic and Montgomery ladder of TweetNaCl (Bernstein, van Gastel,
  Janssen, Lange, Schwabe, Smetsers), https://tweetnacl.cr.yp.to/, public domain.
* Poly1305: the 26-bit limb layout of Andrew Moon's poly1305-donna (32-bit),
  https://github.com/floodyberry/poly1305-donna, public domain or MIT.

SHA-256, HMAC and ChaCha20 follow their specifications (FIPS 180-4, RFC 2104, RFC 8439).

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

* the radio firmware images (`dual-ncp-*.gbl` and `ot-rcp-*.gbl`, each for EFR32MG12 and
  EFR32MG21) contain GSDK components, including precompiled Zigbee, Bluetooth, OpenThread, NVM3
  and RAIL libraries;
* `cpcd`, `iotd`, `ezsp_probe` and `otbr-agent` contain cpc-daemon code (`cpcd` itself, and the
  statically linked `libcpc` client library).

The MSLA permits distributing these in binary form for use with Silicon Labs devices; the AP's
IoT radio is such a device. Silicon Labs' copyright and licence notices that the SDK embeds in
built images are retained. Recipients may use the Silicon Labs binaries only with Silicon Labs
devices, and the MSLA's restrictions, including its prohibition on reverse engineering,
decompiling and disassembling, apply to them.

Per the GSDK's `License.txt`, the precompiled RAIL libraries
(`platform/radio/rail_lib/autogen/librail_release`) and the Bluetooth link layer
(`protocol/bluetooth/bgstack/ll`) are under the Zlib licence instead, as are the GSDK source files
whose headers say so (most of `platform/` and `hardware/`).

## OpenThread border router and RCP images

The Thread programs and images are built from the OpenThread and ot-br-posix copies that GSDK
v4.4.0 ships in `third_party/gecko_sdk/util/third_party/openthread` and
`.../ot-br-posix`, so the host and the radio run the same OpenThread. Both are licensed under the
BSD-3-Clause licence ("Copyright (c) 2016, The OpenThread Authors" and "Copyright (c) 2017, The
OpenThread Authors"); see `licenses/OpenThread-LICENSE.txt`, `licenses/ot-br-posix-LICENSE.txt` and
their `NOTICE` files.

* `otbr-agent` and `ot-ctl` (`scripts/build-otbr.sh`): ot-br-posix with border routing, the border
  agent, the SRP advertising proxy, the DNS-SD discovery proxy and the REST API on, D-Bus and the
  web UI off. `otbr/0001-posix-netif-dummy-interface-without-tun.patch` is applied to OpenThread.
  `otbr-agent` also contains GSDK's CPC vendor interface for the OpenThread POSIX platform
  (`protocol/openthread/platform-abstraction/posix/cpc_interface.cpp`, BSD-3-Clause, The
  OpenThread Authors) and cpc-daemon's `libcpc` (MSLA, see above).
* `ot-rcp-gsdk4.4.0-efr32mg12.gbl` and `ot-rcp-gsdk4.4.0-efr32mg21.gbl` (`scripts/build-thread.sh`):
  GSDK's `protocol/openthread/sample-apps/ot-ncp/ot-rcp` sample with the generated-project changes
  that script lists. Besides OpenThread and GSDK's OpenThread platform layer (BSD-3-Clause), they
  contain Silicon Labs software under the MSLA and Zlib licences (see above) and Mbed TLS (below).

## mDNSResponder

`mdnsd`, and the `dns_sd` client library linked into `otbr-agent`, are Apple's mDNSResponder
1790.80.10, the version ot-br-posix's own bootstrap uses. `scripts/build-otbr.sh` downloads
https://github.com/apple-oss-distributions/mDNSResponder/archive/refs/tags/mDNSResponder-1790.80.10.tar.gz,
checks it against SHA-256 `ffb990231639dfe8c7f0b15d0665206a9013f1aacfb2cbb934eb87f6a57f38b7`, and
applies ot-br-posix's `third_party/mDNSResponder/*.patch` and
`otbr/0002-mdnsresponder-interface-allow-list.patch`.

* The daemon (`mDNSCore`, `mDNSPosix`, and the `mDNSShared` and `DSO` files it uses) is licensed
  under the Apache License 2.0 (`licenses/mDNSResponder-LICENSE.txt`, `licenses/Apache-2.0.txt`).
* The client library (`mDNSShared/dnssd_clientlib.c`, `dnssd_clientstub.c`, `dnssd_ipc.c`,
  `dns_sd.h`) is licensed under the BSD-3-Clause licence, "Copyright (c) 2003-2022 Apple Inc."
  (`licenses/mDNSResponder-dns_sd-BSD-3-Clause.txt`). `otbr-agent` links `dnssd_clientstub.c` and
  `dnssd_ipc.c`; `mdnsd` contains `dnssd_ipc.c`.

## Other bundled libraries

| Library | Version | In | Licence |
|---|---|---|---|
| cJSON (Dave Gamble and cJSON contributors) | 1.7.13 | `otbr-agent` (REST API) | MIT, `licenses/cJSON-LICENSE.txt` |
| http-parser (Joyent, Inc. and other Node contributors) | 2.9.4 | `otbr-agent` (REST API) | MIT, `licenses/http-parser-LICENSE-MIT.txt` |
| Mbed TLS | 2.28.0 (OpenThread's `third_party/mbedtls`) | `otbr-agent` | Apache-2.0, `licenses/Apache-2.0.txt` |
| Mbed TLS | 3.5.0 (GSDK's `util/third_party/mbedtls`) | `dual-ncp-*.gbl`, `ot-rcp-*.gbl` | Apache-2.0, `licenses/Apache-2.0.txt` |
| printf (Marco Paland) | GSDK 4.4.0's `util/third_party/printf` | `dual-ncp-*.gbl` | MIT, `licenses/printf-LICENSE.txt` |
| newlib-nano and libgcc | Arm GNU Toolchain 12.2.rel1 | `dual-ncp-*.gbl`, `ot-rcp-*.gbl` | newlib: the BSD-style licences in its `COPYING.NEWLIB`; libgcc: GPL-3.0 with the GCC Runtime Library Exception |

## GNU C Library and GCC runtime

The AP programs (`cpcd`, `iotd`, `ncpflash`, `ezsp_probe`, `spinel_probe`, `otbr-agent`, `ot-ctl`,
`mdnsd`) are statically linked against the GNU C Library from the Ubuntu `gcc-arm-linux-gnueabi`
cross toolchain, which is licensed under the GNU Lesser General Public License v2.1 or later
(`licenses/LGPL-2.1.txt`). The complete source and build scripts for these programs are in this
repository, its submodules and the pinned mDNSResponder download, so they can be rebuilt and
relinked against a modified library with `scripts/build-ap.sh` and `scripts/build-otbr.sh` (and the
radio images with `scripts/build-firmware.sh` and `scripts/build-thread.sh`).

`otbr-agent` is C++ and contains GCC's libstdc++; every AP program contains GCC's libgcc. Both are
licensed under the GNU General Public License v3 with the GCC Runtime Library Exception 3.1
(`licenses/GPL-3.0.txt`, `licenses/GCC-Runtime-Library-Exception-3.1.txt`), which permits
distributing programs compiled with GCC and combined with them under the programs' own licences.

## Build tools

The build tools downloaded by `scripts/setup-tools.sh` (slc-cli, the Silicon Labs Python and Java
packs, the Arm GNU Toolchain) are not redistributed and keep their own licences; only the
toolchain's runtime libraries linked into the radio images (above) are part of the release.

## Ruckus

This repository contains no Ruckus software. The AP's own firmware, and the vendor IoT gateway
package that this project replaces, remain Ruckus's (CommScope's) property and are neither copied
nor redistributed here.

## No warranty

Flashing an AP's radio is at your own risk. The radio's Gecko bootloader is not modified, and
the vendor gateway restores its own image when it is allowed to run again.
