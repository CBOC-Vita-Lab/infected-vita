#!/usr/bin/env python3
"""soak_report.py NAME : summary of a soak run (private/logs/NAME.err and .out): events, heap headroom, pool use, audio gaps, fatal/stall lines."""
import re, sys
W = '/home/claude/ws/bot/private/logs/'
n = sys.argv[1]
err = open(W + n + '.err', errors='replace').read().split('\n')
out = open(W + n + '.out', errors='replace').read().split('\n')
heap = []; live = []; completes = []; deaths = 0; flips = 0
for l in err:
    m = re.match(r'\[bot\] stats flip (\d+) .* heap_top (\d+) npc_live (\d+) npc_cap (\d+)', l)
    if m:
        f = int(m.group(1)); flips = max(flips, f); heap.append((f, int(m.group(2)))); live.append(int(m.group(3)))
    if '] event mission_complete' in l: completes.append(l)
    if '] event player_dead' in l: deaths += 1
print(f'flips seen {flips} = {flips/60.0/60.0:.1f} min emulated at 60 flips/s')
print(f'missions completed {len(completes)}, player deaths {deaths}')
if heap:
    hs = [h for _, h in heap]
    print(f'heap top-chunk free (bytes): first {hs[0]} min {min(hs)} max {max(hs)} last {hs[-1]}; samples {len(hs)}')
    q = len(hs)//4 or 1
    print('  quarter means:', [int(sum(hs[i:i+q])/len(hs[i:i+q])) for i in range(0, len(hs), q)][:4])
    print(f'npc live: max {max(live)}')
bad = [l for l in err + out if re.search(r'pk_fatal|\[fatal\]|\[stall\]|SIGSEGV|abort|Segmentation|assert|unimplemented|unsupported|\[warn', l)]
print(f'fatal/stall/warn lines: {len(bad)}')
for l in bad[:10]: print('   ', l[:160])
au = []
for l in out:
    m = re.match(r'\[audio\] (\d+)s emu=([\d.]+) frames=(\d+) peak=(\d+) rms=([\d.]+) silent=(\d+) dropped=(\d+) writes=(\d+) restarts=(\d+)', l)
    if m: au.append(tuple(float(x) for x in m.groups()))
if au:
    sil = [a for a in au[2:] if a[5] > 0]; drop = [a for a in au[2:] if a[6] > 0]; quiet = [a for a in au[2:] if a[3] == 0]
    wr = sorted(a[7] for a in au[2:])
    print(f'audio: {len(au)} one-second windows; windows with silent frames {len(sil)}, with dropped frames {len(drop)}, with peak 0 {len(quiet)}; restarts {sum(a[8] for a in au)}; writes/window min {wr[0]:.0f} median {wr[len(wr)//2]:.0f} max {wr[-1]:.0f}')
    for a in sil[:8]: print('   silent window', a)
