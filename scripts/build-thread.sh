#!/usr/bin/env bash
# Build an OpenThread image for an AP's built-in IoT radio.
#
#   scripts/build-thread.sh [mg21|mg12] [ot-rcp|ot-cli-ftd]        (default mg21 ot-rcp)
#   NCP_DEADMAN_MINUTES=3 scripts/build-thread.sh mg12 ot-rcp     lab build with the dead-man reboot
#
# Needs what scripts/build-firmware.sh needs (scripts/setup-tools.sh, the gecko_sdk submodule
# with its Git LFS content).
#
# Source: GSDK 4.4.0 samples in protocol/openthread/sample-apps:
#   ot-rcp      ot-ncp/ot-rcp: the radio co-processor a Thread border router (otbr-agent) drives
#               over Spinel in HDLC-lite framing on the UART
#   ot-cli-ftd  ot-cli/ot-cli-ftd: a full Thread device with the OpenThread command line on the
#               UART, for testing a border router without other Thread hardware
# generated for the same radio boards as the dual NCP (scripts/build-firmware.sh), whose VCOM
# UART, 115200 with hardware flow control, is the one the dual NCP image uses on the AP:
#   mg21  brd4180b, USART0 TX PA5, RX PA6, CTS PA4, RTS PC1
#   mg12  brd4162a, USART0 TX PA0 / RX PA1 location 0, CTS PA2 / RTS PA3 location 30
# with the dual NCP's changes to the generated project (firmware/patch_lfrco.py, and
# firmware/patch_project.py: the reset-cause reservation, the optional dead-man reboot, the mg12
# board fixes) plus OpenThread's reset-to-bootloader (Spinel RESET_BOOTLOADER on the RCP, "reset
# bootloader" on the CLI), the way back to the Gecko bootloader on a radio that has no hardware
# bootloader entry (the R730's EFR32MG12). With the dead-man reboot, any byte from the host counts
# as the host being seen.
#
# Output: out/firmware/<image>-gsdk4.4.0-efr32<target>[-deadman<N>].gbl
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[[ -f "$ROOT/.tools/env.sh" ]] || { echo "run scripts/setup-tools.sh first" >&2; exit 1; }
# shellcheck source=/dev/null
. "$ROOT/.tools/env.sh"

TARGET="${1:-mg21}"
IMAGE="${2:-ot-rcp}"
DEADMAN="${NCP_DEADMAN_MINUTES:-0}"
case "$TARGET" in
  mg21) BOARD=brd4180b ;;
  mg12) BOARD=brd4162a ;;
  *) echo "usage: $0 [mg21|mg12] [ot-rcp|ot-cli-ftd]" >&2; exit 2 ;;
esac
SDK="$ROOT/third_party/gecko_sdk"
APPS="$SDK/protocol/openthread/sample-apps"
case "$IMAGE" in
  ot-rcp) SLCP="$APPS/ot-ncp/ot-rcp.slcp" ;;
  ot-cli-ftd) SLCP="$APPS/ot-cli/ot-cli-ftd.slcp" ;;
  *) echo "usage: $0 [mg21|mg12] [ot-rcp|ot-cli-ftd]" >&2; exit 2 ;;
esac
[[ "$DEADMAN" =~ ^[0-9]+$ ]] || { echo "NCP_DEADMAN_MINUTES must be a number of minutes" >&2; exit 2; }
SUFFIX="efr32$TARGET"
[[ "$DEADMAN" != 0 ]] && SUFFIX="$SUFFIX-deadman$DEADMAN"
VERSION="$(git -C "$ROOT" describe --tags --always 2>/dev/null || echo unknown)"
dirty=0; git -C "$ROOT" diff-index --quiet --ignore-submodules HEAD -- 2>/dev/null || dirty=$?
[[ $dirty == 1 ]] && VERSION="$VERSION-dirty"
VERSION="$VERSION-$TARGET"
[[ "$DEADMAN" != 0 ]] && VERSION="$VERSION-deadman$DEADMAN"

WORK="$ROOT/build/firmware/$IMAGE-$TARGET"
OUT="$ROOT/out/firmware"
GBL="$OUT/$IMAGE-gsdk4.4.0-$SUFFIX.gbl"

[[ -f "$SLCP" ]] || { echo "GSDK not found: git submodule update --init third_party/gecko_sdk" >&2; exit 1; }
LIB="$(find "$SDK/protocol/openthread/libs" -name '*.a' -print -quit)"
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
  --with "$BOARD" \
  --project-file "$SLCP" \
  --export-destination "$WORK" \
  --copy-proj-sources --copy-sdk-sources --new-project \
  --toolchain toolchain_gcc \
  --sdk "$SDK" \
  $SLC_TOOL_PATHS \
  --toolchain-locations="gcc:$ARM_GCC_DIR"

echo "== patch generated project"
"$PYTHON" "$ROOT/firmware/patch_lfrco.py" "$WORK"
"$PYTHON" "$ROOT/firmware/patch_project.py" "$WORK" "$TARGET" "$VERSION" "$DEADMAN" thread

echo "== make"
make -C "$WORK" -f "$IMAGE.Makefile" -j"$(nproc)"
"$ARM_GCC_DIR/bin/arm-none-eabi-size" "$WORK/build/debug/$IMAGE.out"

echo "== package"
"$PYTHON" "$ROOT/firmware/make_gbl.py" "$WORK/build/debug/$IMAGE.hex" "$GBL" --type thread --version 1
