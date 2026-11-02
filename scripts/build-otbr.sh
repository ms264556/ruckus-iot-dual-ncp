#!/usr/bin/env bash
# Build the OpenThread border router for the AP itself: otbr-agent, ot-ctl and its mDNS responder mdnsd, static
# ARMv7 (EABI, soft-float calling convention, like the other AP programs).
#
#   scripts/build-otbr.sh
#
# Needs: apt install gcc-arm-linux-gnueabi g++-arm-linux-gnueabi libc6-dev-armel-cross cmake, and the
# third_party/gecko_sdk submodule (its util/third_party/ot-br-posix and openthread are the versions the RCP images are
# built from, so host and radio speak the same Spinel).
#
# Changes for the AP's firmware:
#   * otbr/0001-posix-netif-dummy-interface-without-tun.patch: the AP kernels have no tun driver, so the Thread
#     interface falls back to a dummy interface, or an empty bridge with ARP off where there are no dummy interfaces
#     (packet socket in, raw IPv6 socket out), with IPv6 enabled on it
#   * settings in /writable/data/thread (the AP's persistent storage; /var/lib is read-only), the control socket under
#     /var/run (there is no /run)
#   * mDNS through Apple's mDNSResponder 1790.80.10 (the version and patches ot-br-posix's bootstrap uses; the
#     download is pinned by SHA-256), built as a static mdnsd plus the dns_sd client library, with its socket and pid
#     file at /var/run/otbr-mdnsd(.pid) so it never meets the firmware's own mdnsd on /var/run/mdnsd, and
#     otbr/0002-mdnsresponder-interface-allow-list.patch: only the interfaces in MDNSD_INTERFACES (iotd passes the
#     infrastructure link), never an IPv4 link-local address
#   * border routing, the border agent (_meshcop._udp), the SRP advertising proxy, the DNS-SD discovery proxy and the
#     REST API (TCP 8081) on
#
# Output: out/otbr/otbr-agent, out/otbr/ot-ctl, out/otbr/mdnsd
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CROSS="${CROSS:-arm-linux-gnueabi-}"
GSDK_TP="$ROOT/third_party/gecko_sdk/util/third_party"
SRC="$ROOT/build/otbr/src"
BUILD="$ROOT/build/otbr/build"
OUT="$ROOT/out/otbr"
MDNS_NAME=mDNSResponder-1790.80.10
MDNS_URL="https://github.com/apple-oss-distributions/mDNSResponder/archive/refs/tags/$MDNS_NAME.tar.gz"
MDNS_SHA=ffb990231639dfe8c7f0b15d0665206a9013f1aacfb2cbb934eb87f6a57f38b7
MDNS_SRC="$ROOT/build/otbr/$MDNS_NAME"
MDNS_OUT="$ROOT/build/otbr/dns_sd"

command -v "${CROSS}g++" >/dev/null || { echo "${CROSS}g++ not found: apt install g++-arm-linux-gnueabi" >&2; exit 1; }
[[ -f "$GSDK_TP/ot-br-posix/CMakeLists.txt" ]] || { echo "GSDK not found: git submodule update --init third_party/gecko_sdk" >&2; exit 1; }

echo "== mDNSResponder ($MDNS_NAME, pinned)"
mkdir -p "$ROOT/.tools/dl"
TARBALL="$ROOT/.tools/dl/$MDNS_NAME.tar.gz"
if ! { [[ -f "$TARBALL" ]] && echo "$MDNS_SHA  $TARBALL" | sha256sum -c --quiet - 2>/dev/null; }; then
  curl -fL --retry 3 -o "$TARBALL.part" "$MDNS_URL"
  echo "$MDNS_SHA  $TARBALL.part" | sha256sum -c --quiet -
  mv "$TARBALL.part" "$TARBALL"
fi
rm -rf "$MDNS_SRC" "$MDNS_OUT"
mkdir -p "$MDNS_SRC" "$MDNS_OUT/lib" "$MDNS_OUT/include"
tar -xzf "$TARBALL" -C "$MDNS_SRC" --strip-components=1
for p in "$GSDK_TP/ot-br-posix/third_party/mDNSResponder"/*.patch "$ROOT/otbr/0002-mdnsresponder-interface-allow-list.patch"; do
  patch -s -d "$MDNS_SRC" -p1 < "$p"
done
sed -i 's|-DPID_FILE=\\"/var/run/mdnsd.pid\\" -DMDNS_UDS_SERVERPATH=\\"/var/run/mdnsd\\"|-DPID_FILE=\\"/var/run/otbr-mdnsd.pid\\" -DMDNS_UDS_SERVERPATH=\\"/var/run/otbr-mdnsd\\"|' \
  "$MDNS_SRC/mDNSPosix/Makefile"
grep -q 'MDNS_UDS_SERVERPATH=\\"/var/run/otbr-mdnsd\\"' "$MDNS_SRC/mDNSPosix/Makefile" || { echo "mdnsd socket path not set" >&2; exit 1; }
# Its sources predate C23 (bool is typedef'd); only mdnsd and the client library are needed, not the dns-sd tool.
make -C "$MDNS_SRC/mDNSPosix" os=linux tls=no CC="${CROSS}gcc -std=gnu11 -ffile-prefix-map=$ROOT=." LD="${CROSS}ld" \
  STRIP="${CROSS}strip" LINKOPTS=-static Daemon libdns_sd
( cd "$MDNS_SRC/mDNSPosix/objects/prod" \
  && "${CROSS}ar" rcs "$MDNS_OUT/lib/libdns_sd.a" dnssd_clientlib.c.so.o dnssd_clientstub.c.so.o dnssd_errstring.c.so.o dnssd_ipc.c.so.o )
cp "$MDNS_SRC/mDNSShared/dns_sd.h" "$MDNS_OUT/include/"

echo "== sources (GSDK ot-br-posix + openthread, patched copy)"
rm -rf "$SRC" "$BUILD"
mkdir -p "$(dirname "$SRC")" "$BUILD" "$OUT"
cp -r "$GSDK_TP/ot-br-posix" "$SRC"
rm -rf "$SRC/third_party/openthread/repo"
cp -r "$GSDK_TP/openthread" "$SRC/third_party/openthread/repo"
patch -d "$SRC/third_party/openthread/repo" -p1 < "$ROOT/otbr/0001-posix-netif-dummy-interface-without-tun.patch"

cat > "$BUILD/toolchain.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_C_COMPILER ${CROSS}gcc)
set(CMAKE_CXX_COMPILER ${CROSS}g++)
set(CMAKE_FIND_ROOT_PATH /usr/arm-linux-gnueabi)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
EOF

# GCC 13 warns about code GSDK 4.4.0's OpenThread was not built with; the project makes warnings errors.
FLAGS="-Wno-maybe-uninitialized -Wno-array-bounds -Wno-stringop-overflow -Wno-stringop-overread -Wno-dangling-pointer"
FLAGS="$FLAGS -Wno-uninitialized -Wno-error -ffile-prefix-map=$ROOT=."
FLAGS="$FLAGS -DOPENTHREAD_POSIX_CONFIG_DAEMON_SOCKET_BASENAME=\\\"/var/run/openthread-%s\\\" -I$MDNS_OUT/include"

echo "== configure"
cmake -S "$SRC" -B "$BUILD" \
  -DCMAKE_TOOLCHAIN_FILE="$BUILD/toolchain.cmake" \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DCMAKE_BUILD_TYPE=MinSizeRel \
  -DBUILD_TESTING=OFF \
  -DOT_THREAD_VERSION=1.3 \
  -DOT_POSIX_SETTINGS_PATH='"/writable/data/thread"' \
  -DOT_FIREWALL=OFF -DOT_READLINE=OFF \
  -DOTBR_MDNS=mDNSResponder -DOTBR_BORDER_AGENT=ON \
  -DOTBR_SRP_ADVERTISING_PROXY=ON -DOTBR_DNSSD_DISCOVERY_PROXY=ON \
  -DOTBR_BORDER_ROUTING=ON -DOTBR_REST=ON \
  -DOTBR_DBUS=OFF -DOTBR_WEB=OFF -DOTBR_NOTIFY_UPSTART=OFF \
  -DCMAKE_C_FLAGS="$FLAGS" -DCMAKE_CXX_FLAGS="$FLAGS" \
  -DCMAKE_EXE_LINKER_FLAGS="-static -L$MDNS_OUT/lib"

echo "== make"
make -C "$BUILD" -j"$(nproc)" otbr-agent ot-ctl
"${CROSS}strip" -o "$OUT/otbr-agent" "$BUILD/src/agent/otbr-agent"
"${CROSS}strip" -o "$OUT/ot-ctl" "$BUILD/third_party/openthread/repo/src/posix/ot-ctl"
"${CROSS}strip" -o "$OUT/mdnsd" "$MDNS_SRC/mDNSPosix/build/prod/mdnsd"
file "$OUT/otbr-agent" "$OUT/ot-ctl" "$OUT/mdnsd"
