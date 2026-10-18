#!/usr/bin/env python3
"""Checks an IFTP pack against a run-time harvest: every texture key the renderer produced (INFECTED_TEX_DUMP json files)
must be answered by the pack, directly (full key) or through a recolour record (image key).

    pack_check.py PACK.bin DUMP_DIR [DUMP_DIR ...] [--catalog private/extract/catalog.json]

Prints the hit / recolour / miss counts and every miss with its size, format and catalog names.
"""
import argparse
import glob
import json
import os
import struct
import sys


def read_index(path):
    with open(path, 'rb') as f:
        head = f.read(16)
        magic, version, count, scale = struct.unpack('<4sIII', head)
        assert magic == b'IFTP' and version == 1, 'not an IFTP v1 pack'
        data = f.read(count * 28)
    recs = {}
    for i in range(count):
        key, off, size, w, h, fmt, mips, flags = struct.unpack_from('<QQIHHBBH', data, i * 28)
        recs[key] = dict(offset=off, size=size, w=w, h=h, fmt=fmt, mips=mips, flags=flags)
    return recs, scale


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('pack')
    ap.add_argument('dumps', nargs='+')
    ap.add_argument('--catalog')
    args = ap.parse_args()
    recs, scale = read_index(args.pack)
    names = {}
    if args.catalog:
        for t in json.load(open(args.catalog))['textures']:
            names[t['key']] = ','.join(t['names'][:2])
    seen = {}
    for d in args.dumps:
        for p in glob.glob(os.path.join(d, '*.json')) + glob.glob(os.path.join(d, '*', '*.json')):
            try:
                j = json.load(open(p))
            except ValueError:
                continue
            seen[j['key']] = j
    direct = recolour = 0
    misses = []
    for key, j in seen.items():
        k = int(key, 16)
        r = recs.get(k)
        if r is not None and not (r['flags'] & 1):
            direct += 1
            continue
        ik = int(j.get('image_key', '0'), 16)
        r = recs.get(ik)
        if r is not None and (r['flags'] & 1):
            recolour += 1
            continue
        misses.append(j)
    print(f'{args.pack}: x{scale}, {len(recs)} records; {len(seen)} distinct run-time textures: direct hits {direct}, '
          f'recolour hits {recolour}, misses {len(misses)}')
    for j in misses:
        print('  MISS', j['key'], f"{j['width']}x{j['height']}", j['format'], names.get(j['key'], '?'),
              'through' if j.get('used_through') else '', '3d' if j.get('used_3d') else '')
    return 1 if misses else 0


if __name__ == '__main__':
    sys.exit(main())
