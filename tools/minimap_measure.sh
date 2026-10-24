#!/bin/bash
# minimap_measure.sh : replays the recorded flips of a level where the minimap palette animates (scripts/minimap_record.sh) with the
# guard of the previous build ("before"), no guard ("off") and the new per-slot guard ("after"), at 2x, full 4x and the mixed pack as 4x.
# Output private/mm/<tag>_<mode>.txt ; summary: scripts/minimap_summary.py
cd /work/ws/all/private
export EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1
GE=${GE:-mm/ge_Mission_5A}
OLD=./bin/ge_replay_before; NEW=../build/desktop/bin/ge_replay
run() { # tag binary mode packdir budget [env]
  tag=$1; bin=$2; mode=$3; packs=$4; budget=$5; envs=${6:-X=1}; name=$7
  out=mm/${tag}_${name}.txt
  [ -s $out ] && grep -q "time per flip" $out && return
  env $envs INFECTED_HD_TRACE=1 nice -n 10 timeout -s KILL 1200 $bin $GE --scale 1 --stats --cpu-time --hd $mode --pack-dir $packs --hd-budget-mb $budget > $out 2>&1
  echo "$tag $name rc=$?"
}
run before $OLD 2x packs_full 256 X=1 2x
run before $OLD 4x packs_full 256 X=1 4xfull
run before $OLD 4x packs_vita 256 X=1 4xmixed
run off $NEW 2x packs_full 256 INFECTED_HD_RECOLOUR_GUARD=off 2x
run off $NEW 4x packs_full 256 INFECTED_HD_RECOLOUR_GUARD=off 4xfull
run off $NEW 4x packs_vita 256 INFECTED_HD_RECOLOUR_GUARD=off 4xmixed
run after $NEW 2x packs_full 256 X=1 2x
run after $NEW 4x packs_full 256 X=1 4xfull
run after $NEW 4x packs_vita 256 X=1 4xmixed
echo finished
