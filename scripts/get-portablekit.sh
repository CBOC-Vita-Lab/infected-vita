#!/usr/bin/env bash
# Makes the PortableKit this port builds against: upstream PortableKit at the commit the port was written on, plus the changes in
# patches/portablekit (the Vita platform hooks, the OpenGL ES renderer work, the settings menu, the HD texture and movie support, ...).
#
#   usage: scripts/get-portablekit.sh [DIR]        (default: third_party/PortableKit of this repository)
#
# The result is a git repository on branch `infected-vita`. The patches rebuild exactly one tree; the script checks that.
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
dest=${1:-$here/third_party/PortableKit}
url=${PORTABLEKIT_URL:-https://github.com/TeamGDB/PortableKit.git}
base=33800c4060c53f3233e08f8ab2cc4590a0872063          # upstream main when the port was last synchronised
tree=a9220349f40d51021d5ed7850e477e51a5171e02          # the tree the patched checkout must have

if [ -e "$dest/.git" ]; then
  echo "$dest already holds a checkout; remove it first to start again" >&2; exit 1
fi
mkdir -p "$(dirname "$dest")"
git clone --quiet "$url" "$dest"
git -C "$dest" checkout --quiet -b infected-vita "$base"
# git am needs a committer; the identity here is only the one written into the local commits
git -C "$dest" -c user.name="infected-vita build" -c user.email="build@invalid" am --quiet --committer-date-is-author-date \
    "$here"/patches/portablekit/*.patch
got=$(git -C "$dest" rev-parse 'HEAD^{tree}')
if [ "$got" != "$tree" ]; then
  echo "warning: the patched tree is $got, expected $tree (upstream history changed, or a patch did not apply as written)" >&2
  exit 1
fi
echo "PortableKit ready in $dest ($(git -C "$dest" rev-list --count "$base..HEAD") patches on $base)"
