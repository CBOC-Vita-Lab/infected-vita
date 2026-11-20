#!/usr/bin/env bash
# Host tests of the Vita application's own code that needs no console: the boot screen's pictures (app/src/boot_art_core.cpp).
#   usage: vita/scripts/test-host.sh PORTABLEKIT_DIR      (a checkout made by scripts/get-portablekit.sh: only its third_party/stb_image*.h are used)
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
pk=${1:?usage: test-host.sh PORTABLEKIT_DIR}
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -I"$pk/third_party" -o "$out/t_boot_art" \
    "$here/selftest/t_boot_art_host.cpp" "$here/app/src/boot_art_core.cpp" -lm
"$out/t_boot_art"
