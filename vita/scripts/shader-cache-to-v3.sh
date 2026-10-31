#!/usr/bin/env bash
# Converts a vitaGL shader cache written by the 1.0 build (vitaGL dca4b9d: v2/{v,f}/XX/<hash>.gxp, hash printed without leading
# zeros) into the layout of vitaGL 9bf3e3f (v3/{v,f}/XX/<hash as 16 digits>.gxp), so that a package built with
# `build.sh --shader-cache DIR` keeps shipping the compiled shaders. The files themselves are not touched: the serialised shader
# format did not change between the two (checked in the diff), only the folder name and the name padding did.
#   usage: vita/scripts/shader-cache-to-v3.sh SRC_SHADER_CACHE_DIR DST_DIR       (SRC has v2/, DST gets v3/; an existing v3/ in SRC is copied too)
set -euo pipefail
src=${1:?usage: shader-cache-to-v3.sh SRC_SHADER_CACHE_DIR DST_DIR}
dst=${2:?usage: shader-cache-to-v3.sh SRC_SHADER_CACHE_DIR DST_DIR}
[ -d "$src" ] || { echo "$src is not a folder" >&2; exit 1; }
[ ! -e "$dst" ] || [ -z "$(ls -A "$dst")" ] || { echo "$dst is not empty" >&2; exit 1; }
n=0
for old in v2 v3; do
  [ -d "$src/$old" ] || continue
  while IFS= read -r f; do
    rel=${f#"$src/$old/"}                      # v/XX/<hash>.gxp  or  f/XX/<hash>.gxp
    kind=${rel%%/*}; rest=${rel#*/}; xx=${rest%%/*}; name=${rest#*/}; hash=${name%.gxp}
    [[ $kind =~ ^[vf]$ && $xx =~ ^[0-9A-F]{2}$ && $hash =~ ^[0-9A-F]{1,16}$ && $name == "$hash.gxp" ]] || { echo "skipping $f (not a cache file)" >&2; continue; }
    hash=$(printf '%016s' "$hash"); hash=${hash// /0}
    [ "${hash:0:2}" = "$xx" ] || { echo "skipping $f (folder $xx does not match hash $hash)" >&2; continue; }
    mkdir -p "$dst/v3/$kind/$xx"
    cp "$f" "$dst/v3/$kind/$xx/$hash.gxp"
    n=$((n + 1))
  done < <(find "$src/$old" -type f -name '*.gxp' | sort)
done
echo "$n shader file(s) written to $dst/v3"
