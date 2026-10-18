#!/bin/bash
# HD residency of the SAME recorded scene (run9 flip 2206) under 2x and 4x in Vita3K: log_sync=1 and a stats line every 3 flips.
V=/home/claude/ws/v3k
SC=/tmp/claude-0/-home-claude/776f7f17-da11-567f-9bb3-a94bf143aa47/scratchpad
for m in 2x 4x; do
  printf 'log_sync=1\nfatal_wait=0\nwatchdog=0\nbench=ge_replay\nbench_dump=dumps_one\nbench_flips=40\nbench_report=10\ngl_stats_every=3\ngfx.hd_textures=%s\n' $m > $SC/benchs_$m.cfg
  $V/v3k.sh --lock-wait 1800 put $SC/benchs_$m.cfg ux0:data/infected/config.txt
  rm -f /home/claude/ws/v3k/vita3k/usr/bin/portable/fs/ux0/data/infected/log.txt
  $V/v3k.sh --lock-wait 1800 run INFC00001 --timeout 150 --elf /home/claude/ws/all/dist/infected.elf > /home/claude/ws/all/private/v3k_stats_$m.out 2>&1
  cp /home/claude/ws/v3k/vita3k/usr/bin/portable/fs/ux0/data/infected/log.txt /home/claude/ws/all/private/v3k_stats_$m.log 2>/dev/null
  echo "$m done"
done
