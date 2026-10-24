#!/bin/bash
# v3k_pack_test.sh PACK [TIMEOUT]: install PACK as ux0:data/infected/texpack_2x.bin and boot the game in Vita3K; prints crash indicators + hdtex tail.
set -u
pack=$1; to=${2:-120}
cd /work/ws/v3k
./v3k.sh --lock-wait 1800 put "$pack" ux0:data/infected/texpack_2x.bin | tail -1
timeout -s KILL $((to+120)) ./v3k.sh --lock-wait 1800 run INFC00001 --timeout "$to" --elf /work/ws/all/dist/infected.elf --tail 1 2>&1 | grep -E "exit reason|CRASH|Unhandled|elapsed" 
echo "uploads: $(grep -c 'hdtex\] upload' runs/last/guest.log)  level0: $(grep -c 'level 0 ' runs/last/guest.log)"
grep hdtex runs/last/guest.log | tail -2 | cut -c1-200
