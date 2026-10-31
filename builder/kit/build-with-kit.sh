#!/usr/bin/env bash
# Prototype of the builder (the Go program will do the same from kit.json): build infected.vpk from the generated C++ using only the kit.
#
#   usage: build-with-kit.sh KIT GEN_DIR OUT_DIR
#     KIT      the unpacked builder kit (the folder with kit.json)
#     GEN_DIR  the generated C++ (lean_NNN.cpp, lean_table.cpp, lean_decls.h, ...)
#     OUT_DIR  receives infected.vpk, eboot.bin and work/ (objects, archive, ELF); work/ is removed unless KEEP=1
#
# Needs bash, python3 (reads kit.json and writes the zip: the Go program does both itself) and the kit. It re-runs itself under
# `env -i PATH=/usr/bin:/bin`: no VITASDK, nothing of the host's compilers. Every command runs with the kit as its working directory.
#
# Environment: KIT_JOBS (parallel compiles and LTO jobs, default 2), KEEP=1, TMPDIR (the LTO link writes about 40 MB there; default OUT/work/tmp),
#   MAKE=PROGRAM       passed on to GCC: the LTO link runs its partitions in parallel through `make` when it finds one and one after the other when not
#                      (MAKE=/nonexistent shows the second case, which is what a PC without make gets),
#   KIT_WORK=DIR       the work folder (default OUT/work); its absolute name is streamed into the LTO objects,
#   KIT_LEAN_LIB=FILE  link this game library instead of compiling GEN_DIR (to test the link alone),
#   KIT_TOOLCHAIN=DIR  use this VitaSDK tree instead of KIT/toolchain (make-kit.sh trace),
#   KIT_TRACE_DIR=DIR  run every step under strace -f and keep one log per step in DIR (make-kit.sh trace).
set -euo pipefail
if [ -z "${KIT_CLEAN:-}" ]; then
  exec env -i PATH=/usr/bin:/bin KIT_CLEAN=1 HOME="${HOME:-/tmp}" KIT_JOBS="${KIT_JOBS:-2}" KEEP="${KEEP:-0}" KIT_TOOLCHAIN="${KIT_TOOLCHAIN:-}" \
       KIT_TRACE_DIR="${KIT_TRACE_DIR:-}" KIT_LEAN_LIB="${KIT_LEAN_LIB:-}" KIT_WORK="${KIT_WORK:-}" ${MAKE:+MAKE="$MAKE"} TMPDIR="${TMPDIR:-}" "$BASH" "$0" "$@"
fi
[ $# = 3 ] || { sed -n 2,13p "$0"; exit 2; }
KIT=$(cd "$1" && pwd); GEN=$(cd "$2" && pwd); mkdir -p "$3"; OUT=$(cd "$3" && pwd)
JOBS=$KIT_JOBS
WORK=${KIT_WORK:-$OUT/work}
rm -rf "$WORK"; mkdir -p "$WORK/obj"
[ -n "$TMPDIR" ] || TMPDIR=$WORK/tmp
mkdir -p "$TMPDIR"; export TMPDIR
cd "$KIT"
ts() { date +%s; }
step() { echo "== $*"; T0=$(ts); }
took() { echo "   $(( $(ts) - T0 )) s"; }

# --- kit.json access (the Go program reads it natively): kj 'python expression on r' prints a value; kjl prints a list, one item per line
kj()  { python3 -I -c 'import json,sys; r=json.load(open("kit.json")); v=eval(sys.argv[1]); print(v)' "$1"; }
kjl() { python3 -I -c 'import json,sys; r=json.load(open("kit.json")); [print(x) for x in eval(sys.argv[1])]' "$1"; }
tool() {   # path of a tool, honouring KIT_TOOLCHAIN
  local p; p=$(kj "r['tools']['$1']")
  if [ -n "$KIT_TOOLCHAIN" ]; then p="$KIT_TOOLCHAIN/${p#toolchain/}"; p=${p%.exe}; else p="$KIT/$p"; fi
  echo "$p"
}
run() {    # run LABEL cmd args...
  local label=$1; shift
  if [ -n "$KIT_TRACE_DIR" ]; then
    mkdir -p "$KIT_TRACE_DIR"
    nice -n 10 strace -f -o "$KIT_TRACE_DIR/$label.log" -e trace=execve,openat,open,stat,newfstatat,statx,access,faccessat2,readlink,readlinkat "$@"
  else
    nice -n 10 "$@"
  fi
}
sub() {    # substitute placeholders: sub KEY=VALUE... < args
  python3 -I -c '
import sys
kv=dict(a.split("=",1) for a in sys.argv[1:])
for l in sys.stdin.read().split("\n"):
    if l=="": continue
    for k,v in kv.items(): l=l.replace("{"+k+"}",v)
    print(l)' "$@"
}

CXX=$(tool cxx); AR=$(tool ar); NM=$(tool nm); READELF=$(tool readelf); ELFCREATE=$(tool elfCreate); FSELF=$(tool fself)
for t in "$CXX" "$AR" "$NM" "$READELF" "$ELFCREATE" "$FSELF"; do [ -x "$t" ] || { echo "kit is incomplete: $t" >&2; exit 1; }; done

# ------------------------------------------------------------------------------------------------------------- compile
LIB=$WORK/liblean_vita_lto.a
if [ -n "${KIT_LEAN_LIB:-}" ]; then LIB=$KIT_LEAN_LIB; echo "== using the game library $LIB (compile and archive skipped)"; else
step "compile the generated C++ ($JOBS jobs)"
mapfile -t CARGS < <(kjl "r['compile']['args']")
srcs=(); for pat in $(kjl "r['compile']['sources']"); do for f in "$GEN"/$pat; do srcs+=("$f"); done; done
echo "   ${#srcs[@]} files"
mkdir -p "$WORK/inc" "$WORK/gen" "$WORK/obj"
cp -R "$KIT/game/include/." "$WORK/inc/"; cp -R "$GEN/." "$WORK/gen/"     # the Go program writes the generated files straight into work/gen
export -f run
cd "$WORK"
printf '%s\0' "${srcs[@]}" | xargs -0 -P "$JOBS" -I{} bash -c '
  stem=$(basename "$1" .cpp); shift
  args=(); for a in "$@"; do a=${a//\{stem\}/$stem}; args+=("$a"); done
  run "compile-$stem" '"$CXX"' "${args[@]}"' _ {} "${CARGS[@]}"
cd "$KIT"
took

# ------------------------------------------------------------------------------------------------------------- archive
step "archive"
rm -f "$LIB"
objs=()
for pat in $(kjl "r['archive']['order']"); do for f in "$WORK"/obj/$pat; do objs+=("$f"); done; done
run archive "$AR" rcs "$LIB" "${objs[@]}"
took
fi

# ------------------------------------------------------------------------------------------------------------- link
link() {   # link LABEL PADOBJ
  mapfile -t LARGS < <(sub lean="$LIB" out="$WORK/infected.elf" jobs="$JOBS" < game/link/link.args)
  if [ -n "${2:-}" ]; then
    local pobj; pobj=$(kj "r['textPad']['object']")
    for i in "${!LARGS[@]}"; do [ "${LARGS[$i]}" = "$pobj" ] && LARGS[$i]=$2; done
  fi
  run "$1" "$CXX" "${LARGS[@]}"
}
step "link (LTO, the long part)"
link link ""
took

step "check for weak pthread functions"
if run nm "$NM" "$WORK/infected.elf" | grep -E "$(kj "r['postLinkCheck']['failIfLineMatches']")"; then
  echo "error: $(kj "r['postLinkCheck']['message']")" >&2; exit 1
fi
took

# ------------------------------------------------------------------------------------------------------------- room after the code segment
step "room after the code segment for vita-elf-create"
need=$(kj "r['textPad']['minFreeBytes']")
free=$(run readelf "$READELF" -lW "$WORK/infected.elf" | python3 -I -c '
import sys
loads=[l.split() for l in sys.stdin if l.strip().startswith("LOAD")]
print(int(loads[1][2],16) - (int(loads[0][2],16) + int(loads[0][4],16)) if len(loads) > 1 else 1 << 20)')
echo "   free: $free bytes (need $need)"
if [ "$free" -lt "$need" ]; then
  pad=$(( $(kj "r['textPad']['hint']") + need + 4096 - free ))
  echo "   the hint was not enough: padding .text by $pad bytes and linking again"
  mapfile -t PARGS < <(kjl "r['textPad']['fix']['args']")
  PARGS=("${PARGS[@]//\{pad\}/$pad}"); PARGS=("${PARGS[@]//\{padObject\}/$WORK/text_pad.o}")
  run textpad "$CXX" "${PARGS[@]}"
  link link2 "$WORK/text_pad.o"
fi
took

# ------------------------------------------------------------------------------------------------------------- vita-elf-create, vita-make-fself
step "vita-elf-create"
mapfile -t EARGS < <(kjl "r['elfCreate']['args']")
EARGS=("${EARGS[@]//\{elf\}/$WORK/infected.elf}"); EARGS=("${EARGS[@]//\{velf\}/$WORK/infected.velf}")
run elfcreate "$ELFCREATE" "${EARGS[@]}"
took
step "vita-make-fself"
mapfile -t FARGS < <(kjl "r['fself']['args']")
FARGS=("${FARGS[@]//\{velf\}/$WORK/infected.velf}"); FARGS=("${FARGS[@]//\{eboot\}/$OUT/eboot.bin}")
run fself "$FSELF" "${FARGS[@]}"
took

# ------------------------------------------------------------------------------------------------------------- the package
step "package"
python3 -I - "$KIT" "$OUT" <<'PY'
import json, os, sys, zipfile
kit, out = sys.argv[1:3]
r = json.load(open(os.path.join(kit, 'kit.json')))
path = os.path.join(out, r['vpk']['zip'])
if os.path.exists(path): os.remove(path)
with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
    for name in r['vpk']['entries']:
        src = os.path.join(out, 'eboot.bin') if name == r['vpk']['eboot'] else os.path.join(kit, 'game', 'vpk', name)
        zi = zipfile.ZipInfo(name, (2026, 1, 1, 0, 0, 0))
        zi.compress_type = zipfile.ZIP_DEFLATED
        zi.external_attr = 0o644 << 16
        with open(src, 'rb') as f:
            z.writestr(zi, f.read())
print('   ' + path)
PY
took
[ "${KEEP:-0}" = 1 ] || rm -rf "$WORK"
echo "@done $OUT/infected.vpk"
