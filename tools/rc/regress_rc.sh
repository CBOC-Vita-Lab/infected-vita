#!/bin/bash
# regress_rc.sh SET... : release-candidate replay regression. Replays each GE dump set with the merged build at ORIGINAL options
# (--port baseline: every option off), once on the calling thread and once with --threaded, and compares the md5 of every picture
# with private/regress/<set>/integ.md5 (the integration baseline from before the options existed). Pictures are deleted afterwards.
M=${M:-/home/claude/ws/all/build/desktop/bin/ge_replay}
for SET in "$@"; do
  D=/home/claude/ws/render/private/dumps/$SET
  OUT=/home/claude/ws/all/private/regress_rc/$SET
  BASE=/home/claude/ws/all/private/regress/$SET/integ.md5
  mkdir -p $OUT
  for mode in merged merged_thr; do
    opts=(--port baseline); [ $mode = merged_thr ] && opts+=(--threaded)
    rm -rf $OUT/$mode; mkdir -p $OUT/$mode
    s=$(date +%s)
    EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 nice -n 10 timeout -s KILL 3000 $M $D "${opts[@]}" --png $OUT/$mode/f.png > $OUT/$mode.txt 2>&1
    rc=$?
    (cd $OUT/$mode && md5sum *.png) > $OUT/$mode.md5
    rm -rf $OUT/$mode
    if diff -q $BASE $OUT/$mode.md5 > /dev/null; then r="IDENTICAL to integ"; else r="DIFFERS from integ: $(diff $BASE $OUT/$mode.md5 | grep -c '^<') pictures"; fi
    echo "$SET $mode rc=$rc $(( $(date +%s) - s )) s $(wc -l < $OUT/$mode.md5) pictures: $r"
  done
done
echo RC-REGRESS-DONE
