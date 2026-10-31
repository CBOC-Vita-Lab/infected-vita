#!/usr/bin/env bash
# Makes the builder kit: the smallest set of files that lets a player's computer turn the recompiled game into infected.vpk without
# installing VitaSDK. See builder/kit/README in the commit message of this directory's history, kit.json "notes", and the lists in lists/.
#
# The kit is made in two parts, so that CI can build the target part once (Linux) and the host part once per host OS:
#
#   TARGET part (the same for every host OS; made on Linux with a full VitaSDK):
#       toolchain/arm-vita-eabi/{include,lib}, toolchain/lib/gcc/arm-vita-eabi/15.2.0/{crt*.o,libgcc.a,include,...}   (lists/target-files.txt)
#       game/include   game/profile.json   game/link (prebuilt objects + libraries + link.args)   game/vpk   licenses/   target.json
#   HOST part (programs that run on the player's computer; copied from a VitaSDK tree OF THAT OS):
#       the programs and data files in lists/host-files.txt, with the OS's file names (see below)
#
#   make-kit.sh target --sdk SDK --sysroot DIR --repo DIR --out DIR [--build-dir DIR] [--shader-cache DIR] [--text-pad N] [--work DIR]
#   make-kit.sh host   --target-dir DIR --out DIR [--host-sdk DIR] [--host-os linux|windows|mac] [--host-arch amd64|arm64] [--strip PROG]
#   make-kit.sh pack   --kit DIR --out FILE.tar.gz [--exe infected-build]                      (the archive that is published; `zip` is the old name)
#   make-kit.sh all    --sdk SDK --sysroot DIR --repo DIR --out FILE.tar.gz [--work DIR] [--exe FILE] [--shader-cache DIR]
#                                                                                              (target + host for this Linux + pack)
#   make-kit.sh trace  --target-dir DIR --sdk SDK --gen DIR [--work DIR]                     (rewrites lists/*.txt from strace logs)
#
#   --sdk         the VitaSDK tree (the "vitasdk" folder) used for the target part; --host-sdk defaults to it.
#   --sysroot     the third-party libraries (vitaGL, vitaShaRK, SceShaccCgExt, taiHEN stubs, math-neon in lib/, FFmpeg in
#                 ffmpeg-nopt/ffmpeg-vita/lib): the result of vita/scripts/build-sysroot.sh.
#   --repo        this repository (with third_party/PortableKit made by scripts/get-portablekit.sh).
#   --build-dir   an existing, built CMake tree of vita/app (vita/app/build.sh --lean-lib ...) to take objects and VPK files from;
#                 without it the script builds one with a stub game library (no disc is needed or touched; the link of the stub is
#                 only there to produce the object files and link.txt).
#   --shader-cache DIR   a vitaGL shader cache (the folder that holds v3/) to ship in the VPK, passed on to vita/app/build.sh (target, without --build-dir).
#   --exe FILE    pack: put this infected-build program into the kit folder first (as infected-build, or infected-build.exe for a windows kit).
#   --text-pad    bytes of .text padding the real game link needs (default: builder/kit/text-pad.txt).
#
# Host file names per OS (lists/host-files.txt has one entry per file, keyed by the Linux path relative to the VitaSDK root):
#     prog PATH   executable: PATH on linux and mac, PATH.exe on windows
#     lib  PATH   shared library or plugin, given without extension: PATH.so (linux), PATH.dll (windows), PATH.dylib (mac; .so as fallback)
#     data PATH   a file a host program reads (same name everywhere), taken from the host SDK so that it matches the program.
# Windows: the VitaSDK for x86_64-w64-mingw32 programs depend only on system DLLs; nothing is added. A host SDK whose programs need
# further DLLs must have them added to lists/host-files.txt as "data" lines.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
TOOL="python3 -I $HERE/kit_tool.py"
cmd=${1:-}; shift || true
SDK=""; HOST_SDK=""; SYSROOT=""; REPO=""; OUT=""; BUILD=""; TEXTPAD=""; WORK=""; TARGET_DIR=""; HOST_OS=""; HOST_ARCH=""; KIT=""; GEN=""; STRIP=""; SHADER_CACHE=""; EXE=""
while [ $# -gt 0 ]; do
  case "$1" in
    --sdk) SDK=$2; shift 2;; --host-sdk) HOST_SDK=$2; shift 2;; --sysroot) SYSROOT=$2; shift 2;; --repo) REPO=$2; shift 2;;
    --out) OUT=$2; shift 2;; --build-dir) BUILD=$2; shift 2;; --text-pad) TEXTPAD=$2; shift 2;; --work) WORK=$2; shift 2;;
    --target-dir) TARGET_DIR=$2; shift 2;; --host-os) HOST_OS=$2; shift 2;; --host-arch) HOST_ARCH=$2; shift 2;;
    --kit) KIT=$2; shift 2;; --gen) GEN=$2; shift 2;; --strip) STRIP=$2; shift 2;; --shader-cache) SHADER_CACHE=$2; shift 2;; --exe) EXE=$2; shift 2;;
    *) echo "unknown argument $1" >&2; exit 2;;
  esac
done
BOOT_SHA256=952d4bb2c08209f4cbc0020d95c4cc601e9944cad5c7ade4c1d060fe8daab1bc   # BOOT.BIN of Infected ULUS-10054 1.02 (docs/BUILDING.md)
abspath() { (cd "$1" && pwd); }
say() { echo "== $*"; }

# ================================================================================================================ target part
do_target() {
  [ -n "$SDK" ] && [ -n "$SYSROOT" ] && [ -n "$REPO" ] && [ -n "$OUT" ] || { echo "target needs --sdk --sysroot --repo --out" >&2; exit 2; }
  SDK=$(abspath "$SDK"); SYSROOT=$(abspath "$SYSROOT"); REPO=$(abspath "$REPO"); mkdir -p "$OUT"; OUT=$(abspath "$OUT")
  PK=$REPO/third_party/PortableKit
  [ -d "$PK/include/psprecomp" ] || { echo "no PortableKit in $PK: run scripts/get-portablekit.sh" >&2; exit 1; }
  [ -n "$TEXTPAD" ] || TEXTPAD=$(tr -d '[:space:]' < "$HERE/text-pad.txt")
  export VITASDK=$SDK VITA_SYSROOT=$SYSROOT
  WORK=${WORK:-$OUT.work}; mkdir -p "$WORK"; WORK=$(abspath "$WORK")
  STRIP_TOOL=${STRIP:-$SDK/bin/arm-vita-eabi-strip}     # strips ARM objects (the host strip may not know ARM)

  if [ -z "$BUILD" ]; then
    say "building the application objects with a stub game library (no disc involved)"
    mkdir -p "$WORK/stub"
    printf 'void (*const g_lean_entry[1])(void) = {0};\nconst unsigned g_lean_entry_count = 0;\n' > "$WORK/stub/stub.c"
    "$SDK/bin/arm-vita-eabi-gcc" -c "$WORK/stub/stub.c" -o "$WORK/stub/stub.o"
    rm -f "$WORK/stub/liblean_stub_lto.a"; "$SDK/bin/arm-vita-eabi-ar" rcs "$WORK/stub/liblean_stub_lto.a" "$WORK/stub/stub.o"
    # same source paths in every build of the kit: the prefix map keeps the repository and work folder out of assert strings
    CFLAGS="-ffile-prefix-map=$REPO=/src -ffile-prefix-map=$WORK=/build" CXXFLAGS="-ffile-prefix-map=$REPO=/src -ffile-prefix-map=$WORK=/build" \
      "$REPO/vita/app/build.sh" --lean-lib "$WORK/stub/liblean_stub_lto.a" --portablekit "$PK" --outdir "$WORK/build" --dist "$WORK/dist" \
        ${SHADER_CACHE:+--shader-cache "$SHADER_CACHE"}
    BUILD=$WORK/build
  fi
  BUILD=$(abspath "$BUILD")
  [ -f "$BUILD/CMakeFiles/infected.dir/link.txt" ] || { echo "$BUILD is not a built tree of vita/app" >&2; exit 1; }

  say "kit skeleton"
  rm -rf "$OUT/toolchain" "$OUT/game" "$OUT/licenses" "$OUT/target.json"
  mkdir -p "$OUT/toolchain" "$OUT/game/include" "$OUT/licenses"

  if [ -f "$HERE/lists/target-files.txt" ]; then
    say "toolchain target files ($(grep -vc '^#' "$HERE/lists/target-files.txt"))"
    while read -r f; do
      case "$f" in ''|'#'*) continue;; esac
      mkdir -p "$OUT/toolchain/$(dirname "$f")"; cp -p "$SDK/$f" "$OUT/toolchain/$f"
    done < "$HERE/lists/target-files.txt"
  else
    echo "note: lists/target-files.txt does not exist yet (run: make-kit.sh trace); the toolchain/ part is left empty"
  fi

  say "game/include, profile.json"
  # the headers the generated C++ includes: the closure found by the compiler itself
  printf '#include "lean_abi.h"\n' > "$WORK/probe.cpp"
  "$SDK/bin/arm-vita-eabi-g++" -std=c++20 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb -I"$PK/include" -I"$REPO/recomp" -MM "$WORK/probe.cpp" \
      | tr ' ' '\n' | grep -E "^($PK/include|$REPO/recomp)/" | while read -r h; do
        case "$h" in "$PK/include/"*) rel=${h#"$PK/include/"};; *) rel=$(basename "$h");; esac
        mkdir -p "$OUT/game/include/$(dirname "$rel")"; cp -p "$h" "$OUT/game/include/$rel"
      done
  cp -p "$REPO/recomp/infected_profile.json" "$OUT/game/profile.json"

  say "game/link"
  # the pad object: compiled with the pad the real game needs (the stub link is smaller than the game, so its own pad is wrong)
  $TOOL game-link --build "$BUILD" --repo "$REPO" --pk "$PK" --kit "$OUT" --strip "$STRIP_TOOL"
  padobj=$WORK/text_pad.o
  $TOOL pad-object --build "$BUILD" --sdk "$SDK" --repo "$REPO" --pad "$TEXTPAD" --out "$padobj" --strip "$STRIP_TOOL"
  cp -p "$padobj" "$OUT/game/link/obj/infected/src/text_pad.cpp.o"
  cp -p "$REPO/vita/app/src/text_pad.cpp" "$OUT/game/link/text_pad.cpp"

  say "game/vpk"
  $TOOL game-vpk --build "$BUILD" --kit "$OUT"

  say "licenses"
  cp -p "$HERE"/licenses/* "$OUT/licenses/"

  say "recipe"
  lean_flags=$(LEAN_PRINT_FLAGS=1 LEAN_LTO=1 "$REPO/recomp/build_lib.sh" vita "$REPO/recomp")
  repo_commit=$(git -C "$REPO" rev-parse HEAD 2>/dev/null || echo unknown)
  pk_commit=$(sed -n 's/^base=\([0-9a-f]*\).*/\1/p' "$REPO/scripts/get-portablekit.sh")
  extra=$(python3 -I -c 'import json,sys; print(json.dumps({"source": {"repository": "https://github.com/CBOC-Vita-Lab/infected-vita", "commit": sys.argv[1], "portablekit": "https://github.com/TeamGDB/PortableKit commit " + sys.argv[2] + " + patches/portablekit (scripts/get-portablekit.sh)"}}))' "$repo_commit" "$pk_commit")
  $TOOL recipe --build "$BUILD" --kit "$OUT" --lean-flags "$lean_flags" --extra "$extra" --boot-sha256 "$BOOT_SHA256" --sdk-note "VitaSDK $(grep -m1 -o 'Built at.*' "$SDK/version_info.txt")"
  du -sh "$OUT/game" "$OUT/toolchain" 2>/dev/null
}

# ================================================================================================================ host part
do_host() {
  [ -n "$TARGET_DIR" ] && [ -n "$OUT" ] || { echo "host needs --target-dir --out" >&2; exit 2; }
  TARGET_DIR=$(abspath "$TARGET_DIR")
  HOST_OS=${HOST_OS:-linux}
  HOST_ARCH=${HOST_ARCH:-amd64}
  [ -n "$HOST_SDK" ] || HOST_SDK=$SDK
  [ -n "$HOST_SDK" ] || { echo "host needs --host-sdk (or --sdk)" >&2; exit 2; }
  HOST_SDK=$(abspath "$HOST_SDK")
  rm -rf "$OUT"; mkdir -p "$OUT"; OUT=$(abspath "$OUT")
  cp -a "$TARGET_DIR/." "$OUT/"
  say "host files for $HOST_OS/$HOST_ARCH from $(basename "$(dirname "$HOST_SDK")")/$(basename "$HOST_SDK")"
  strip_arg=()
  [ -n "$STRIP" ] && strip_arg=(--strip "$STRIP")
  if [ -z "$STRIP" ] && [ "$HOST_OS" != windows ] && command -v strip >/dev/null && [ "$HOST_OS" = "$(uname -s | tr A-Z a-z | sed 's/darwin/mac/')" ]; then strip_arg=(--strip strip); fi
  $TOOL host-copy --list "$HERE/lists/host-files.txt" --host-sdk "$HOST_SDK" --kit "$OUT" --os "$HOST_OS" "${strip_arg[@]}"
  # provenance of the exact SDK copy the host programs came from (the fixed part of SOURCES.txt says where the sources are)
  {
    echo
    echo "4. Generated section: the VitaSDK tree the programs of this kit ($HOST_OS, $HOST_ARCH) were copied from"
    echo "   ----------------------------------------------------------------------------------------------"
    sed 's/^/   /' "$HOST_SDK/version_info.txt" 2>/dev/null || true
    [ -f "$HOST_SDK/share/vdpm/release-info.txt" ] && sed 's/^/   /' "$HOST_SDK/share/vdpm/release-info.txt"
    echo
    echo "   SHA-256 of every shipped file: kit.json \"files\"."
  } >> "$OUT/licenses/SOURCES.txt"
  $TOOL finalize --kit "$OUT" --os "$HOST_OS" --arch "$HOST_ARCH"
  du -sh "$OUT" "$OUT/toolchain" "$OUT/game" | cat
}

do_pack() {
  [ -n "$KIT" ] && [ -n "$OUT" ] || { echo "pack needs --kit --out" >&2; exit 2; }
  $TOOL pack --kit "$KIT" --out "$OUT" ${EXE:+--exe "$EXE"}
}

# ================================================================================================================ evidence
do_trace() {
  [ -n "$TARGET_DIR" ] && [ -n "$SDK" ] && [ -n "$GEN" ] || { echo "trace needs --target-dir --sdk --gen" >&2; exit 2; }
  SDK=$(abspath "$SDK"); TARGET_DIR=$(abspath "$TARGET_DIR"); GEN=$(abspath "$GEN")
  WORK=${WORK:-$TARGET_DIR.trace}; rm -rf "$WORK"; mkdir -p "$WORK"; WORK=$(abspath "$WORK")
  command -v strace >/dev/null || { echo "install strace (apt install strace)" >&2; exit 1; }
  # a kit without toolchain/: the steps run with the full SDK in its place, under strace
  rm -rf "$WORK/kit"; mkdir -p "$WORK/kit"
  cp -a "$TARGET_DIR/game" "$TARGET_DIR/licenses" "$TARGET_DIR/target.json" "$WORK/kit/"
  $TOOL finalize --kit "$WORK/kit" --os linux --arch amd64 >/dev/null
  say "running the build steps under strace (the long part)"
  KIT_TOOLCHAIN=$SDK KIT_TRACE_DIR=$WORK/logs KEEP=0 TMPDIR=$WORK/tmp "$HERE/build-with-kit.sh" "$WORK/kit" "$GEN" "$WORK/out"
  mkdir -p "$HERE/lists"
  $TOOL trace --sdk "$SDK" --out "$HERE/lists" "$WORK"/logs/*.log
}

case "$cmd" in
  target) do_target;;
  host) do_host;;
  pack|zip) do_pack;;
  trace) do_trace;;
  all)
    [ -n "$OUT" ] || { echo "all needs --out FILE.tar.gz" >&2; exit 2; }
    packout=$OUT
    OUT=${WORK:-$(dirname "$packout")/kit-work}/target; do_target
    TARGET_DIR=$OUT; OUT=${WORK:-$(dirname "$packout")/kit-work}/kit; do_host
    KIT=$OUT; OUT=$packout; do_pack;;
  *) sed -n 2,28p "$0"; exit 2;;
esac
