#!/usr/bin/env bash
# Downloads the pinned VitaSDK build for one host, checks its SHA-256 and unpacks it. The SDK is the exact one the port is built
# with (GCC 15.2.0, "Built at 2026-09-26 08:30:00"); the checksums are the SHA256SUMS asset of the release below.
#
#   usage: .github/scripts/fetch-vitasdk.sh TRIPLE DEST      TRIPLE: x86_64-linux-gnu | x86_64-w64-mingw32 | x86_64-apple-darwin | arm64-apple-darwin
#   result: DEST/vitasdk/ (print it with: echo "$DEST/vitasdk")
set -euo pipefail
tag=sdk-snapshot-20260926.790.1
base=https://github.com/vitasdk/autobuilds/releases/download/$tag
triple=${1:?triple}; dest=${2:?dest}
case "$triple" in
  x86_64-linux-gnu)     file=vitasdk-x86_64-linux-gnu-2026-09-26_08-31-53.tar.bz2;  sha=87b111ff350c1f36bde1c2923e57467c1f86b044d54bc616631260486f2a512c;;
  x86_64-w64-mingw32)   file=vitasdk-x86_64-w64-mingw32-2026-09-26_08-49-25.tar.bz2; sha=0d32ac3f8c91f98e964457365e9857ee8f9f244cc7ea3faf53c65607cd1261eb;;
  x86_64-apple-darwin)  file=vitasdk-x86_64-apple-darwin-2026-09-26_08-49-19.tar.bz2; sha=b0b0f49faabf7cd20f9a473870babec4ee3247a0ec2b8061144037369af7e8ac;;
  arm64-apple-darwin)   file=vitasdk-arm64-apple-darwin-2026-09-26_08-31-03.tar.bz2;  sha=ad735e8938b1d81689180237f94ec129c44922a3d5ff1ef3b5f21e790e1faa91;;
  *) echo "unknown triple $triple" >&2; exit 2;;
esac
mkdir -p "$dest"
curl -fsSL --retry 4 --retry-delay 5 -o "$dest/sdk.tar.bz2" "$base/$file"
got=$(python3 -I -c 'import hashlib,sys; print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "$dest/sdk.tar.bz2")
if [ "$got" != "$sha" ]; then echo "VitaSDK $file: SHA-256 is $got, expected $sha" >&2; exit 1; fi
tar -xjf "$dest/sdk.tar.bz2" -C "$dest"
rm -f "$dest/sdk.tar.bz2"
[ -x "$dest/vitasdk/bin/arm-vita-eabi-gcc" ] || [ -x "$dest/vitasdk/bin/arm-vita-eabi-gcc.exe" ] || { echo "no arm-vita-eabi-gcc in $dest/vitasdk" >&2; exit 1; }
echo "VitaSDK $triple ready in $dest/vitasdk ($(grep -m1 'Built at' "$dest/vitasdk/version_info.txt"))"
