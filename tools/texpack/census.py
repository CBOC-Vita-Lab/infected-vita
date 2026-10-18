#!/usr/bin/env python3
"""Whole-disc texture census, from extract.py's catalog.json (and optionally a run-time harvest for the draw kinds).

    census.py --extract private/extract [--rt private/rt]

Prints: distinct textures / images by format and by size; UI/2D versus world (and character, fx) counts and pixels;
what the draw kinds say (observed 2D/3D) versus the name rule; palette variants per image; texel memory at 1x/2x/4x.
"""
import argparse
import collections
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import classify  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--extract', required=True)
    ap.add_argument('--rt')
    args = ap.parse_args()
    cat = json.load(open(os.path.join(args.extract, 'catalog.json')))
    observed = classify.load_observed(args.rt) if args.rt else {}
    groups = collections.OrderedDict()
    for t in cat['textures']:
        groups.setdefault(t['image'], []).append(t)
    print('streams', len(cat['census']['streams']), 'rasters', cat['census']['rasters_in_streams'],
          'distinct textures (full key)', len(cat['textures']), 'distinct images', len(groups))

    by_fmt = collections.Counter(t['format'] for t in cat['textures'])
    print('\nformat of distinct textures:', dict(by_fmt))
    print('format of distinct images  :', dict(collections.Counter(ts[0]['format'] for ts in groups.values())))

    print('\nsize buckets (distinct images; longest side):')
    buckets = collections.Counter()
    for ts in groups.values():
        b = max(ts[0]['width'], ts[0]['height'])
        buckets['<=4' if b <= 4 else '8-16' if b <= 16 else '32' if b <= 32 else '64' if b <= 64 else '128' if b <= 128 else '256' if b <= 256 else '512+'] += 1
    for k in ['<=4', '8-16', '32', '64', '128', '256', '512+']:
        print(f'  {k:6s} {buckets[k]:5d}')

    rows = collections.defaultdict(lambda: {'images': 0, 'textures': 0, 'pixels': 0, 'variants': 0})
    named = collections.Counter()
    agree = collections.Counter()
    for image, ts in groups.items():
        names = sorted({n for t in ts for n in t['names']}) or ['?']
        keys = [t['key'] for t in ts]
        cls = classify.image_class(names, keys, observed)
        by_name = classify.name_class(names[0])
        if observed:
            kinds = {observed[k] for k in keys if k in observed}
            agree[(by_name, '+'.join(sorted(kinds)) or 'unseen')] += 1
        r = rows[cls]
        r['images'] += 1
        r['textures'] += len(ts)
        r['pixels'] += ts[0]['width'] * ts[0]['height']
        r['variants'] += len(ts)
    print('\nclass         images textures      pixels   share of pixels')
    tot = sum(r['pixels'] for r in rows.values())
    for cls in ('ui', 'world', 'character', 'fx'):
        r = rows[cls]
        print(f'  {cls:10s} {r["images"]:6d} {r["textures"]:8d} {r["pixels"]:11d}   {100.0 * r["pixels"] / tot:5.1f}%')
    print(f'  {"all":10s} {sum(r["images"] for r in rows.values()):6d} {sum(r["textures"] for r in rows.values()):8d} {tot:11d}')
    ui = rows['ui']['images']
    print(f'\nUI/2D versus world: {ui} images 2D (HUD, menus, fonts, logos, backgrounds) versus '
          f'{rows["world"]["images"] + rows["character"]["images"] + rows["fx"]["images"]} images drawn in 3D '
          f'({rows["world"]["images"]} world, {rows["character"]["images"]} character, {rows["fx"]["images"]} fx)')
    if observed:
        print('\nname rule versus observed draw kind (images):')
        for (nm, kind), n in sorted(agree.items()):
            print(f'  name={nm:10s} observed={kind:12s} {n}')

    print('\npalette variants per image (distinct textures sharing one texel image):')
    hist = collections.Counter(len(ts) for ts in groups.values())
    for k in sorted(hist):
        print(f'  {k:4d} variant(s): {hist[k]} images')
    top = sorted(groups.values(), key=lambda ts: -len(ts))[:8]
    for ts in top:
        print(f'  {len(ts):4d}  {ts[0]["width"]}x{ts[0]["height"]} {ts[0]["format"]}  {",".join(sorted({n for t in ts for n in t["names"]})[:3])}')

    px = sum(ts[0]['width'] * ts[0]['height'] for ts in groups.values())
    print(f'\nunique texel pixels {px / 1e6:.2f} M; RGBA8 at 2x {px * 4 * 4 / 1048576:.0f} MiB, at 4x {px * 16 * 4 / 1048576:.0f} MiB '
          f'(before DXT and mips); DXT1 is 0.5 byte/px and DXT5 1 byte/px, +1/3 for mips')


if __name__ == '__main__':
    main()
