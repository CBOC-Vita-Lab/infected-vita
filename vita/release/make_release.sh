#!/bin/bash
# Assembles the release folder from a finished Vita build.
#   usage: vita/release/make_release.sh [WORKSPACE]          (default: the directory that holds this repository)
# Reads   WORKSPACE/dist/infected.vpk, WORKSPACE/dist/infected.elf (from vita/app/build.sh, with --dist WORKSPACE/dist),
#         WORKSPACE/private/packs_vita/{texpack_2x.bin,texpack_4x.bin}   (texpack_4x.bin = the MIXED 4x pack, 102 MB; never the 274 MB full pack),
#         WORKSPACE/private/dumps_m1/        (GE dump set: pages.pack + flip_N.ge, see docs/history/release-candidate/HARDWARE-TEST.md part C),
#         WORKSPACE/private/ms0.pristine/    (a bot profile: becomes bench_profile_ms0),
#         the documents INSTALL.md, docs/SETTINGS.md and docs/KNOWN-ISSUES.md of this repository.
# A maintainer's tool: the packs, dumps and profile it packs are made from the owner's own disc and are not in the repository.
# Writes  WORKSPACE/dist/release/  (infected.vpk, ux0_data_infected/, INSTALL.md, SETTINGS.md, KNOWN-ISSUES.md). infected.elf stays in WORKSPACE/dist/ beside the folder.
# Nothing under WORKSPACE/dist/release is kept from an earlier run.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
ws=${1:-$(cd "$here/../../.." && pwd)}
dist=$ws/dist
rel=$dist/release
for f in "$dist/infected.vpk" "$dist/infected.elf" "$ws/private/packs_vita/texpack_2x.bin" "$ws/private/packs_vita/texpack_4x.bin" \
         "$ws/private/dumps_m1/pages.pack" "$ws/private/ms0.pristine"; do
  [ -e "$f" ] || { echo "missing: $f" >&2; exit 1; }
done
rm -rf "$rel"
mkdir -p "$rel/ux0_data_infected"
cp "$dist/infected.vpk" "$rel/infected.vpk"
cp "$ws/private/packs_vita/texpack_2x.bin" "$rel/ux0_data_infected/texpack_2x.bin"
cp "$ws/private/packs_vita/texpack_4x.bin" "$rel/ux0_data_infected/texpack_4x.bin"
cp -r "$ws/private/dumps_m1" "$rel/ux0_data_infected/dumps"
cp -r "$ws/private/ms0.pristine" "$rel/ux0_data_infected/bench_profile_ms0"
cp "$here/config.txt.example" "$rel/ux0_data_infected/config.txt.example"
cp "$here/../../INSTALL.md" "$rel/INSTALL.md"
for d in SETTINGS.md KNOWN-ISSUES.md; do cp "$here/../../docs/$d" "$rel/$d"; done
# sanity: the 4x pack must not be the 274 MB full pack
sz=$(stat -c %s "$rel/ux0_data_infected/texpack_4x.bin")
[ "$sz" -lt 160000000 ] || { echo "texpack_4x.bin is $sz bytes: that is the full 4x pack, not the mixed one" >&2; exit 1; }
echo "release folder: $rel"
(cd "$rel" && find . -type f | sort | while read -r f; do printf '%12d  %s\n' "$(stat -c %s "$f")" "$f"; done | awk '{print} {s+=$1} END {printf "%12d  total (%d files)\n", s, NR}' | sed -e '/dumps\/flip_/d' )
echo "dumps: $(ls "$rel/ux0_data_infected/dumps" | wc -l) files, $(du -sb "$rel/ux0_data_infected/dumps" | cut -f1) bytes"
echo "elf (beside the folder): $(stat -c %s "$dist/infected.elf") bytes  $dist/infected.elf"
