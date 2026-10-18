#!/bin/bash
# regress.sh SET [FLIPS] : replay one GE dump set with the predecessors' binaries and the merged one, compare the PNGs by md5.
#   integ  = /home/claude/ws/integ/build/bin/ge_replay                  (before the options existed)
#   gfx    = /home/claude/ws/gfx/build/bin/ge_replay --port baseline    (every option off)
#   merged = build/desktop/bin/ge_replay --port baseline                (this tree), and the same with --threaded
D=/home/claude/ws/render/private/dumps/$1; F=${2:-}
OUT=/home/claude/ws/all/private/regress/$1
M=/home/claude/ws/all/build/desktop/bin/ge_replay
mkdir -p $OUT
run() { # name binary options...
  n=$1; b=$2; shift 2
  [ -f $OUT/$n.md5 ] && return
  rm -rf $OUT/$n; mkdir -p $OUT/$n
  s=$(date +%s)
  EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 nice -n 10 timeout -s KILL 3000 $b $D ${F:+--flips $F} "$@" --png $OUT/$n/f.png > $OUT/$n.txt 2>&1
  rc=$?
  (cd $OUT/$n && md5sum *.png) > $OUT/$n.md5.tmp && mv $OUT/$n.md5.tmp $OUT/$n.md5
  echo "$1 $n rc=$rc $(( $(date +%s) - s )) s $(wc -l < $OUT/$n.md5) pictures"
  rm -rf $OUT/$n/*.png.keep
}
run integ /home/claude/ws/integ/build/bin/ge_replay
run gfx /home/claude/ws/gfx/build/bin/ge_replay --port baseline
run merged $M --port baseline
run merged_thr $M --port baseline --threaded
for v in gfx merged merged_thr; do
  if diff -q $OUT/integ.md5 $OUT/$v.md5 > /dev/null; then echo "$1: $v == integ ($(wc -l < $OUT/integ.md5) pictures)"; else echo "$1: $v DIFFERS from integ:"; diff $OUT/integ.md5 $OUT/$v.md5 | head -6; fi
done
