#!/bin/bash
# Same-state HD test in Vita3K: bench=ge_replay on ONE recorded mission-1 flip (run9 flip 2206), HD off / 2x / 4x through the config.txt
# override gfx.hd_textures. Needs ux0:data/infected/dumps_one/{flip_002206.ge,pages.pack} and the packs in the emulator (private).
S=/work/ws/all/scripts
V=/work/ws/v3k
SC=/tmp/claude-0/-home-claude/776f7f17-da11-567f-9bb3-a94bf143aa47/scratchpad
printf 'wait 55\nshot same_state_a\nwait 30\nshot same_state_b\n' > $SC/bench_same.keys
for m in off 2x 4x; do
  printf 'log_sync=0\nfatal_wait=0\nwatchdog=0\nbench=ge_replay\nbench_dump=dumps_one\nbench_flips=400\nbench_report=100\ngl_stats_every=50\ngfx.hd_textures=%s\n' $m > $SC/bench_$m.cfg
  $V/v3k.sh --lock-wait 1800 put $SC/bench_$m.cfg ux0:data/infected/config.txt
  rm -f /work/ws/v3k/vita3k/usr/bin/portable/fs/ux0/data/infected/log.txt
  $V/v3k.sh --lock-wait 1800 run INFC00001 --timeout 400 --elf /work/ws/all/dist/infected.elf --keys-file $SC/bench_same.keys > /work/ws/all/private/v3k_same_$m.out 2>&1
  cp /work/ws/v3k/vita3k/usr/bin/portable/fs/ux0/data/infected/log.txt /work/ws/all/private/v3k_same_$m.log 2>/dev/null
  d=$(ls -d $V/runs/2026*-INFC00001 | tail -1)
  mkdir -p /work/ws/all/private/v3k_same_$m && cp $d/shots/*.png /work/ws/all/private/v3k_same_$m/ 2>/dev/null
  echo "$m done: $d" 
done
