#!/usr/bin/env python3 -I
"""Summary of scripts/minimap_measure.sh: one line per run (tag, mode): recolours and their time, refusals, resident HD memory, the replay's
per-flip time (mean, median, 95th percentile, worst) and how much of it the recolours are."""
import glob, os, re, sys
d = sys.argv[1] if len(sys.argv) > 1 else '/work/ws/all/private/mm'
rows = []
for f in sorted(glob.glob(os.path.join(d, '*_*.txt'))):
    name = os.path.basename(f)[:-4]
    if name.split('_')[0] not in ('before', 'off', 'after'):
        continue
    text = open(f, errors='replace').read()
    m = re.search(r'hd textures (\S+): keyed (\d+).*?hits (\d+) \(recoloured (\d+), ([\d.]+) ms\).*?budget refusals (\d+), recolour refusals (\d+).*?resident (\d+) textures ([\d.]+) MB \(peak ([\d.]+) MB\)', text)
    if not m:
        continue
    flips = [float(x) for x in re.findall(r'^flip \d+:.*? flip ([\d.]+) ms', text, re.M)]
    s = sorted(flips)
    n = len(s)
    rec_n, rec_ms = int(m.group(4)), float(m.group(5))
    rows.append((name, rec_n, rec_ms, rec_ms / rec_n if rec_n else 0.0, int(m.group(7)), int(m.group(6)), float(m.group(10)),
                 sum(flips) / n, s[n // 2], s[int(n * 0.95)], s[-1], n))
print('%-14s %9s %10s %9s %9s %8s %9s | %7s %7s %7s %7s  (ms per flip; software GL on a shared host)' %
      ('run', 'recolours', 'rebuild ms', 'ms each', 'refused', 'budget', 'peak MB', 'mean', 'median', 'p95', 'worst'))
for r in rows:
    print('%-14s %9d %10.1f %9.2f %9d %8d %9.1f | %7.2f %7.2f %7.2f %7.2f  (%d flips)' % r)
