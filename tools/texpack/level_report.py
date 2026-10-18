#!/usr/bin/env python3
"""Per level (disc stream) memory and coverage of the texture packs, from the catalog and the pack indexes.

    level_report.py --catalog private/extract/catalog.json --pack 2x=private/packs/texpack_2x.bin --pack 4x=... [--rt DIR ...]

For every stream: the number of distinct textures the stream holds, the GPU bytes of all of them with no pack (the original decoded to RGBA8, what the renderer
uploads), with the 2x and the 4x pack (the blob size of a DXT record, RGBA8 plus mips for a recolour record: what the GPU holds), how many of them have
a pack record (directly or as a recolour image) and, with --rt (INFECTED_TEX_DUMP directories of real runs), how many were seen drawn.
The sums are an upper bound of what one level can have resident (the renderer's cache evicts, and a level never draws all of its textures at once).
"""
import argparse
import collections
import glob
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pack_check import read_index  # noqa: E402


def gpu_bytes(rec, w, h):
    if rec is None:
        return 0
    if rec['flags'] & 1:   # recolour: rebuilt as RGBA8 with a full mip chain
        return rec['w'] * rec['h'] * 4 * 4 // 3
    return rec['size']


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--catalog', required=True)
    ap.add_argument('--pack', action='append', required=True, help='LABEL=path (repeatable)')
    ap.add_argument('--rt', nargs='*', default=[], help='run-time harvest directories (json files)')
    args = ap.parse_args()
    cat = json.load(open(args.catalog))['textures']
    packs = {}
    for spec in args.pack:
        label, path = spec.split('=', 1)
        packs[label] = read_index(path)[0]
    seen = set()
    for d in args.rt:
        for f in glob.glob(os.path.join(d, '*.json')):
            try:
                seen.add(json.load(open(f))['key'])
            except Exception:
                pass
    streams = collections.defaultdict(list)
    for t in cat:
        for s in t['streams']:
            streams[s].append(t)

    def rec_for(pack, t):
        r = pack.get(int(t['key'], 16))
        if r is None and t.get('image_key'):
            r = pack.get(int(t['image_key'], 16))
            if r is not None and not (r['flags'] & 1):
                r = None
        return r

    labels = list(packs)
    print(f'{"stream":16s} {"textures":>8s} {"orig MB":>8s} ' + ' '.join(f'{l + " MB":>8s}' for l in labels) + f' {"covered":>8s} {"seen":>6s}')
    total = collections.Counter()
    rows = []
    for name in sorted(streams):
        ts = streams[name]
        orig = sum(t['width'] * t['height'] * 4 for t in ts)
        sums = {l: sum(gpu_bytes(rec_for(p, t), t['width'], t['height']) for t in ts) for l, p in packs.items()}
        covered = sum(1 for t in ts if all(rec_for(p, t) is not None for p in packs.values()))
        nseen = sum(1 for t in ts if t['key'] in seen)
        rows.append((name, len(ts), orig, sums, covered, nseen))
        print(f'{name:16s} {len(ts):8d} {orig / 1048576:8.1f} ' + ' '.join(f'{sums[l] / 1048576:8.1f}' for l in labels) + f' {covered:8d} {nseen:6d}')
    miss = [t for t in cat if any(rec_for(p, t) is None for p in packs.values())]
    print(f'\ncatalog textures without a pack record (any pack): {len(miss)} of {len(cat)}')
    by = collections.Counter(t['class'] for t in miss)
    print('  by class:', dict(by))
    for t in miss[:30]:
        print('  ', t['key'], t['width'], t['height'], t['format'], t['names'][:2], t.get('alpha'))
    big = sorted(rows, key=lambda r: -r[3].get(labels[-1], 0))[:5]
    print('\nlargest streams at ' + labels[-1] + ': ' + ', '.join(f'{r[0]} {r[3][labels[-1]] / 1048576:.0f} MB' for r in big))
    return 0


if __name__ == '__main__':
    sys.exit(main())
