#!/usr/bin/env python3
"""Tabulates ge_replay --stats logs of the dump corpus (one log per dump and HD mode, see pipeline.sh / verify_corpus.sh).

    corpus_report.py --logs private/verify --dumps /path/to/dumps

Per dump: flips, textures cached (the working set of the dump's window), GPU texture memory with the pack off / 2x / 4x
(the 'cached textures' figure of the replayer: replacements are counted at their DXT or RGBA8 size), hits, misses,
recoloured hits and the key cost. The worst case per mode is marked.
"""
import argparse
import glob
import os
import re


def parse(path):
    text = open(path, errors='replace').read()
    r = {}
    m = re.search(r'(\d+) cached textures \((\d+) KiB\)', text)
    if m:
        r['textures'], r['kib'] = int(m.group(1)), int(m.group(2))
    m = re.search(r'hd textures (\w+): keyed (\d+) \(([\d.]+) us each\), hits (\d+) \(recoloured (\d+), ([\d.]+) ms\), misses (\d+), no key (\d+), '
                  r'render-target copies skipped (\d+), budget refusals (\d+), recolour refusals (\d+), failed (\d+); resident (\d+) textures ([\d.]+) MB '
                  r'\(peak ([\d.]+) MB\), read ([\d.]+) MB, lookup ([\d.]+) ms, upload ([\d.]+) ms', text)
    if m:
        keys = ['mode', 'keyed', 'key_us', 'hits', 'recoloured', 'recolour_ms', 'misses', 'nokey', 'skipped', 'budget', 'recolour_refused',
                'failed', 'resident_n', 'resident_mb', 'peak_mb', 'read_mb', 'lookup_ms', 'upload_ms']
        r.update(dict(zip(keys, m.groups())))
    m = re.search(r'whole flip ([\d.]+) ms', text)
    if m:
        r['flip_ms'] = float(m.group(1))
    r['ok'] = 'exit=0' in text
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--logs', required=True)
    ap.add_argument('--dumps', required=True)
    args = ap.parse_args()
    names = sorted({os.path.basename(p).split('.')[0] for p in glob.glob(os.path.join(args.logs, '*.off.log'))},
                   key=lambda n: (re.sub(r'\d+', '', n), int(re.sub(r'\D', '', n) or 0)))
    rows = []
    for n in names:
        d = {m: parse(os.path.join(args.logs, f'{n}.{m}.log')) for m in ('off', '2x', '4x')
             if os.path.exists(os.path.join(args.logs, f'{n}.{m}.log'))}
        flips = len(glob.glob(os.path.join(args.dumps, n, 'flip_*.ge')))
        rows.append((n, flips, d))
    print(f'{"dump":8s} {"flips":>5s} {"tex":>4s} | {"off MiB":>8s} {"2x MiB":>8s} {"4x MiB":>8s} | {"4x hits":>7s} {"miss":>5s} {"recol":>5s} {"skip":>4s} {"key us":>6s} {"refus":>5s}')
    worst = {'off': (0, ''), '2x': (0, ''), '4x': (0, '')}
    tot = {'hits': 0, 'misses': 0, 'recol': 0, 'keyed': 0, 'skipped': 0}
    for n, flips, d in rows:
        def mib(m):
            return d[m]['kib'] / 1024.0 if m in d and 'kib' in d[m] else float('nan')
        h = d.get('4x', {})
        for m in worst:
            if m in d and 'kib' in d[m] and mib(m) > worst[m][0]:
                worst[m] = (mib(m), n)
        print(f'{n:8s} {flips:5d} {d.get("off", {}).get("textures", 0):4d} | {mib("off"):8.1f} {mib("2x"):8.1f} {mib("4x"):8.1f} | '
              f'{h.get("hits", "-"):>7s} {h.get("misses", "-"):>5s} {h.get("recoloured", "-"):>5s} {h.get("skipped", "-"):>4s} {h.get("key_us", "-"):>6s} {h.get("budget", "-"):>5s}')
        if h:
            tot['hits'] += int(h['hits']); tot['misses'] += int(h['misses']); tot['recol'] += int(h['recoloured'])
            tot['keyed'] += int(h['keyed']); tot['skipped'] += int(h['skipped'])
    print('\nworst case GPU texture memory: ' + ', '.join(f'{m} {v[0]:.1f} MiB ({v[1]})' for m, v in worst.items()))
    print('4x totals over the corpus (per-dump caches): keyed %(keyed)d, hits %(hits)d (recoloured %(recol)d), misses %(misses)d, 8888 skipped %(skipped)d' % tot)


if __name__ == '__main__':
    main()
