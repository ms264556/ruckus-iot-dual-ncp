#!/usr/bin/env bash
# Build the AP-side programs: cpcd, iotd, ncpflash and ezsp_probe.
#
#   scripts/build-ap.sh
#
# Static ARMv7 (EABI, soft-float calling convention) binaries for the AP's Linux, which has a
# uClibc userland: static linking keeps them independent of it.
# Needs: apt install gcc-arm-linux-gnueabi libc6-dev-armel-cross cmake, and the third_party/cpc-daemon submodule.
#
# cpcd is Silicon Labs' cpc-daemon 4.9.1 built without link encryption (matching the
# firmware) and with its sockets under /tmp (the AP's /dev/shm is read-only).
#
# Output: out/ap/ (binaries, cpcd.conf, start.sh, iotd.conf.example)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CROSS="${CROSS:-arm-linux-gnueabi-}"
CPC_SRC="$ROOT/third_party/cpc-daemon"
CPC_BUILD="$ROOT/build/cpc-daemon"
OUT="$ROOT/out/ap"

command -v "${CROSS}gcc" >/dev/null || { echo "${CROSS}gcc not found: apt install gcc-arm-linux-gnueabi" >&2; exit 1; }
[[ -f "$CPC_SRC/CMakeLists.txt" ]] || { echo "cpc-daemon not found: git submodule update --init third_party/cpc-daemon" >&2; exit 1; }

PREFIX_MAP="-ffile-prefix-map=$ROOT=."

echo "== cpc-daemon (cpcd, libcpc.a)"
rm -rf "$CPC_BUILD"
cmake -S "$CPC_SRC" -B "$CPC_BUILD" \
  -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_SYSTEM_PROCESSOR=arm \
  -DCMAKE_C_COMPILER="${CROSS}gcc" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_FLAGS="-static -O2 $PREFIX_MAP" \
  -DCMAKE_EXE_LINKER_FLAGS=-static \
  -DBUILD_SHARED_LIBS=OFF \
  -DENABLE_ENCRYPTION=FALSE \
  -DCPC_SOCKET_DIR=/tmp \
  -DUSE_LEGACY_GPIO_SYSFS=OFF \
  >/dev/null
cmake --build "$CPC_BUILD" -j"$(nproc)" --target cpcd cpc

echo "== iotd, ncpflash, ezsp_probe"
make -C "$ROOT/ap" clean
make -C "$ROOT/ap" -j"$(nproc)" \
  CROSS="$CROSS" \
  CPC_INC="$CPC_SRC/lib" \
  CPC_LIB="$CPC_BUILD/libcpc.a" \
  EXTRA_CFLAGS="$PREFIX_MAP"

echo "== collect"
rm -rf "$OUT"
mkdir -p "$OUT"
cp "$CPC_BUILD/cpcd" "$ROOT/ap/iotd/iotd" "$ROOT/ap/tools/ncpflash" "$ROOT/ap/tools/ezsp_probe" "$OUT/"
"${CROSS}strip" "$OUT/cpcd" "$OUT/iotd" "$OUT/ncpflash" "$OUT/ezsp_probe"
cp "$ROOT/ap/cpcd.conf" "$ROOT/ap/start.sh" "$ROOT/ap/iotd.conf.example" "$OUT/"
chmod 755 "$OUT/start.sh"
file "$OUT"/cpcd "$OUT"/iotd "$OUT"/ncpflash "$OUT"/ezsp_probe 2>/dev/null || ls -l "$OUT"
