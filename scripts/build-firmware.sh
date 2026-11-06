#!/usr/bin/env bash
# Build the dual Zigbee + Bluetooth NCP image for an AP's built-in IoT radio.
#
#   scripts/build-firmware.sh [mg21|mg12|mg21spi]        (default mg21)
#   NCP_DEADMAN_MINUTES=3 scripts/build-firmware.sh mg12   lab build with the dead-man reboot
#
# Needs: scripts/setup-tools.sh run once, and the third_party/gecko_sdk submodule checked
# out with its Git LFS content.
#
# Source: GSDK 4.4.0 sample protocol/zigbee/app/ncp/sample-app/ncp-dmp/zigbee_ncp-ble_ncp-uart
# (Zigbee NCP and Bluetooth NCP multiplexed over CPC on one UART), generated for a radio
# board whose UART pins match the AP's radio:
#   mg21  brd4180b (EFR32MG21A020F1024IM32, USART0 on PA5/PA6 with RTS/CTS): R560 and the
#         other EFR32MG21 APs
#   mg12  brd4162a (EFR32MG12, Series 1, USART0 TX PA0 / RX PA1 location 0, CTS PA2 / RTS
#         PA3 location 30): the R730's radio. The dev kit's SPI flash shutdown and SWO
#         output are switched off, since they drive pins the module may use otherwise.
#   mg21spi  brd4180b with the CPC SPI secondary driver (cpc_secondary_driver_spi_usart, instance
#         exp) in place of the UART one: the R650's EFR32MG21 (an MGM210L-style layout) on the AP's
#         SPI, USART2 with the host's MOSI on PC1, MISO on PC0, CLK PC2, CS PC3, and the IRQ
#         (HOST_INT) on PA4 (firmware/patch_project.py edits the generated exp config).
# with these changes to the generated project:
#   * clock tree on the internal LFRCO (firmware/patch_lfrco.py)
#   * CPC link encryption off (SL_CPC_SECURITY_ENABLED 0; cpcd is built without it)
#   * the CPC application version and optional dead-man reboot (firmware/patch_project.py,
#     firmware/ncp_app_hooks.c), and for mg12 DCDC bypass, PA supply, no VCOM enable pin,
#     no packet trace
# The GBL's EraseProg address is the image's link address (0x4000 on mg21, 0x0 on mg12).
#
# Output: out/firmware/dual-ncp-gsdk4.4.0-efr32<target>[-deadman<N>].gbl
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[[ -f "$ROOT/.tools/env.sh" ]] || { echo "run scripts/setup-tools.sh first" >&2; exit 1; }
# shellcheck source=/dev/null
. "$ROOT/.tools/env.sh"

TARGET="${1:-mg21}"
DEADMAN="${NCP_DEADMAN_MINUTES:-0}"
case "$TARGET" in
  mg21) BOARD=brd4180b ;;
  mg12) BOARD=brd4162a ;;
  mg21spi) BOARD=brd4180b ;;
  *) echo "usage: $0 [mg21|mg12|mg21spi]" >&2; exit 2 ;;
esac
[[ "$DEADMAN" =~ ^[0-9]+$ ]] || { echo "NCP_DEADMAN_MINUTES must be a number of minutes" >&2; exit 2; }
SUFFIX="efr32$TARGET"
SLC_WITH="$BOARD"
SLC_WITHOUT=()
if [[ "$TARGET" == mg21spi ]]; then
  SLC_WITH="$BOARD,cpc_secondary_driver_spi_usart:exp"
  SLC_WITHOUT=(--without cpc_secondary_driver_uart_usart)
fi
[[ "$DEADMAN" != 0 ]] && SUFFIX="$SUFFIX-deadman$DEADMAN"
# Not `describe --dirty`: it also inspects submodules, and CI restores the GSDK from a cache without its
# .git/modules entry, which makes describe fail and stamped release images "unknown".
VERSION="$(git -C "$ROOT" describe --tags --always 2>/dev/null || echo unknown)"
dirty=0; git -C "$ROOT" diff-index --quiet --ignore-submodules HEAD -- 2>/dev/null || dirty=$?
[[ $dirty == 1 ]] && VERSION="$VERSION-dirty"
VERSION="$VERSION-$TARGET"
[[ "$DEADMAN" != 0 ]] && VERSION="$VERSION-deadman$DEADMAN"

SDK="$ROOT/third_party/gecko_sdk"
PROJECT=zigbee_ncp-ble_ncp-uart
SLCP="$SDK/protocol/zigbee/app/ncp/sample-app/ncp-dmp/$PROJECT.slcp"
WORK="$ROOT/build/firmware/$PROJECT-$TARGET"
OUT="$ROOT/out/firmware"
GBL="$OUT/dual-ncp-gsdk4.4.0-$SUFFIX.gbl"

[[ -f "$SLCP" ]] || { echo "GSDK not found: git submodule update --init third_party/gecko_sdk" >&2; exit 1; }
LIB="$(find "$SDK/protocol/bluetooth/lib" -name '*.a' -print -quit)"
if [[ -z "$LIB" || "$(head -c 7 "$LIB")" != '!<arch>' ]]; then
  echo "GSDK libraries are Git LFS pointers: install git-lfs and run 'git -C third_party/gecko_sdk lfs pull'" >&2
  exit 1
fi

echo "== slc generate"
rm -rf "$WORK"
mkdir -p "$WORK" "$OUT"
# shellcheck disable=SC2086
"$SLC" generate \
  --trust-totality \
  --with "$SLC_WITH" "${SLC_WITHOUT[@]}" \
  --project-file "$SLCP" \
  --export-destination "$WORK" \
  --copy-proj-sources --copy-sdk-sources --new-project \
  --toolchain toolchain_gcc \
  --sdk "$SDK" \
  $SLC_TOOL_PATHS \
  --toolchain-locations="gcc:$ARM_GCC_DIR"

echo "== patch generated project"
"$PYTHON" "$ROOT/firmware/patch_lfrco.py" "$WORK"
SEC="$WORK/config/sl_cpc_security_config.h"
sed -i -E 's/(#define SL_CPC_SECURITY_ENABLED)[[:space:]]+1\b/\1 0/' "$SEC"
grep -Eq '#define SL_CPC_SECURITY_ENABLED[[:space:]]+0\b' "$SEC" || { echo "failed to disable CPC security in $SEC" >&2; exit 1; }
echo "CPC security disabled"
"$PYTHON" "$ROOT/firmware/patch_project.py" "$WORK" "$TARGET" "$VERSION" "$DEADMAN"

echo "== make"
make -C "$WORK" -f "$PROJECT.Makefile" -j"$(nproc)"
"$ARM_GCC_DIR/bin/arm-none-eabi-size" "$WORK/build/debug/$PROJECT.out"

echo "== package"
"$PYTHON" "$ROOT/firmware/make_gbl.py" "$WORK/build/debug/$PROJECT.hex" "$GBL" --type zigbee --version 1
