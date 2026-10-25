#!/usr/bin/env bash
# Build the dual Zigbee + Bluetooth NCP image for the AP's EFR32MG21.
#
#   scripts/build-firmware.sh
#
# Needs: scripts/setup-tools.sh run once, and the third_party/gecko_sdk submodule checked
# out with its Git LFS content.
#
# Source: GSDK 4.4.0 sample protocol/zigbee/app/ncp/sample-app/ncp-dmp/zigbee_ncp-ble_ncp-uart
# (Zigbee NCP and Bluetooth NCP multiplexed over CPC on one UART), generated for the
# brd4180b radio board (EFR32MG21A020F1024IM32, USART0 on PA5/PA6 with RTS/CTS), with
# two changes to the generated project:
#   * clock tree on the internal LFRCO (firmware/patch_lfrco.py)
#   * CPC link encryption off (SL_CPC_SECURITY_ENABLED 0; cpcd is built without it)
#
# Output: out/firmware/dual-ncp-gsdk4.4.0-efr32mg21.gbl
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[[ -f "$ROOT/.tools/env.sh" ]] || { echo "run scripts/setup-tools.sh first" >&2; exit 1; }
# shellcheck source=/dev/null
. "$ROOT/.tools/env.sh"

SDK="$ROOT/third_party/gecko_sdk"
PROJECT=zigbee_ncp-ble_ncp-uart
SLCP="$SDK/protocol/zigbee/app/ncp/sample-app/ncp-dmp/$PROJECT.slcp"
WORK="$ROOT/build/firmware/$PROJECT"
OUT="$ROOT/out/firmware"
GBL="$OUT/dual-ncp-gsdk4.4.0-efr32mg21.gbl"

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
  --with brd4180b \
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

echo "== make"
make -C "$WORK" -f "$PROJECT.Makefile" -j"$(nproc)"
"$ARM_GCC_DIR/bin/arm-none-eabi-size" "$WORK/build/debug/$PROJECT.out"

echo "== package"
"$PYTHON" "$ROOT/firmware/make_gbl.py" "$WORK/build/debug/$PROJECT.hex" "$GBL" --type zigbee --version 1
