#!/usr/bin/env bash
# Collect the build outputs into dist/ with a reproducible AP bundle and SHA256SUMS.
#
#   scripts/package.sh
#
# Run after build-firmware.sh and build-ap.sh. ap-armv7.tar.gz depends only on the files'
# contents and the commit: names are sorted, owner and group are 0, permissions are fixed
# (755 programs and scripts, 644 configuration), every timestamp is the commit time
# (SOURCE_DATE_EPOCH, from git when unset), and gzip stores no name or time.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/out"
DIST="$ROOT/dist"
EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "$ROOT" log -1 --format=%ct)}"

for f in "$OUT/firmware/dual-ncp-gsdk4.4.0-efr32mg21.gbl" "$OUT/firmware/dual-ncp-gsdk4.4.0-efr32mg12.gbl" "$OUT/ap/iotd"; do
  [[ -f "$f" ]] || { echo "build first: scripts/build-firmware.sh mg21, scripts/build-firmware.sh mg12 and scripts/build-ap.sh ($f missing)" >&2; exit 1; }
done
if ls "$OUT"/firmware/*deadman*.gbl >/dev/null 2>&1; then
  echo "out/firmware holds a dead-man lab build; remove it before packaging a release" >&2
  exit 1
fi

rm -rf "$DIST"
mkdir -p "$DIST"

# Fixed permissions before archiving, so the umask of the build machine does not matter.
for f in "$OUT"/ap/*; do
  case "$(basename "$f")" in
    *.conf|*.example) chmod 644 "$f" ;;
    *) chmod 755 "$f" ;;
  esac
done
chmod 755 "$OUT/ap"

tar -C "$OUT" --format=gnu --sort=name --mtime="@$EPOCH" \
    --owner=0 --group=0 --numeric-owner -cf - ap \
  | gzip -9 -n > "$DIST/ap-armv7.tar.gz"

cp "$OUT"/firmware/*.gbl "$OUT"/ap/* "$DIST/"
(cd "$DIST" && sha256sum -- * > SHA256SUMS && cat SHA256SUMS)
