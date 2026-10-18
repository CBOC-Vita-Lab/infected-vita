#!/usr/bin/env bash
# Builds every self-test flavour and collects the packages in dist/ (git-ignored).
#   dist/infected_selftest.vpk                          release vitaGL, FFmpeg nopt, ATTRIBUTE2=12   <- run this first
#   dist/variants/infected_selftest-{attr2-0,debug,fast,store,ffmpeg-pt}.vpk
# Needs the libraries from scripts/build-vitagl.sh {release,debug,fast,store} and scripts/build-ffmpeg.sh {nopt,pt}.
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
dist=$here/dist
mkdir -p "$dist/variants"
b() {   # b <outname> <vita/build.sh args...>
  local name=$1; shift
  echo "=== $name: vita/build.sh $*"
  "$here/vita/build.sh" "$@" --outdir "$here/build/selftest-$name" | tail -3
}
b release --vitagl release --ffmpeg nopt --attr2 12
cp "$here/build/selftest-release/infected_selftest.vpk" "$dist/infected_selftest.vpk"
for v in debug fast store; do
  b "$v" --vitagl "$v" --ffmpeg nopt --attr2 12
  cp "$here/build/selftest-$v/infected_selftest.vpk" "$dist/variants/infected_selftest-$v.vpk"
done
b ffmpeg-pt --vitagl release --ffmpeg pt --attr2 12
cp "$here/build/selftest-ffmpeg-pt/infected_selftest.vpk" "$dist/variants/infected_selftest-ffmpeg-pt.vpk"
b attr2-0 --vitagl release --ffmpeg nopt --attr2 0
cp "$here/build/selftest-attr2-0/infected_selftest.vpk" "$dist/variants/infected_selftest-attr2-0.vpk"
ls -l "$dist/infected_selftest.vpk" "$dist"/variants/*.vpk
