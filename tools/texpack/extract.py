#!/usr/bin/env python3
"""Extract every texture of the disc's streams, keyed by the renderer's replacement key.

    extract.py --levels DIR_OF_ZPM --texkey build/bin/texkey --out private/extract
    extract.py --iso Infected.iso --texkey ...        (reads /PSP_GAME/USRDIR/IMAGE/levels from the image first)

Writes OUT/orig/<key>.png (the renderer's RGBA decode of each distinct raster), OUT/rasters.raw (the IFRW records,
in catalog order, for texkey and the equality tests) and OUT/catalog.json:

    {"textures": [{"key", "width", "height", "format", "image": <id of the texel data>, "names": [...],
                   "streams": [...], "palette_variants": n, "alpha": {...}, "raw_index": i}, ...], "census": {...}}

The key and the picture come from `texkey batch`, which links the same texture_key.cpp and texture_decode.cpp as the
renderer: nothing here reimplements the hash or the decode.
"""
import argparse
import collections
import hashlib
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rwstream  # noqa: E402

FORMAT_NAMES = {0: 'rgb565', 4: 'clut4', 5: 'clut8'}


def iso_levels(iso: str, workdir: str) -> str:
    out = os.path.join(workdir, 'levels')
    if os.path.isdir(out) and any(f.endswith('.zpm') for f in os.listdir(out)):
        return out
    # PortableKit's tools/extract_iso.py: third_party/PortableKit of this repository (scripts/get-portablekit.sh), or where $PORTABLEKIT points
    root = os.environ.get('PORTABLEKIT') or os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'third_party', 'PortableKit')
    tool = os.path.join(root, 'tools', 'extract_iso.py')
    os.makedirs(workdir, exist_ok=True)
    subprocess.run([sys.executable, tool, iso, workdir, '/PSP_GAME/USRDIR/IMAGE/levels'], check=True)
    found = os.path.join(workdir, 'PSP_GAME', 'USRDIR', 'IMAGE', 'levels')
    os.makedirs(out, exist_ok=True)
    for f in os.listdir(found):
        os.replace(os.path.join(found, f), os.path.join(out, f))
    return out


def classify(name: str) -> str:
    n = name.lower()
    if n.startswith(('ui_', 'icon_', 'button_', 'hud_', 'font', 'menu', 'aa_', 'logo', 'cutscene_', 'splash')):
        return 'ui'
    return 'other'


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--levels')
    ap.add_argument('--iso')
    ap.add_argument('--texkey', required=True)
    ap.add_argument('--out', required=True)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    levels = args.levels or iso_levels(args.iso, os.path.join(args.out, 'iso'))

    records = {}   # sha1 of the IFRW record -> dict
    per_stream = {}
    for path in sorted(f for f in os.listdir(levels) if f.endswith('.zpm')):
        stream = path.replace('.stream.zpm', '')
        rasters = rwstream.rasters_in(os.path.join(levels, path))
        per_stream[stream] = len(rasters)
        for r in rasters:
            rec = rwstream.raw_record(r)
            h = hashlib.sha1(rec).hexdigest()
            e = records.get(h)
            if e is None:
                image = hashlib.sha1(r.texels + bytes([r.width & 255, r.width >> 8, r.height & 255, r.height >> 8, r.bits])).hexdigest()[:16]
                e = records[h] = {'record': rec, 'width': r.width, 'height': r.height, 'bits': r.bits, 'image': image,
                                  'names': {}, 'streams': set(), 'platforms': set(), 'uses': 0}
            e['names'][r.name] = e['names'].get(r.name, 0) + 1
            e['streams'].add(stream)
            e['platforms'].add(r.platform)
            e['uses'] += 1
        print(f'{stream}: {len(rasters)} rasters', file=sys.stderr)

    order = sorted(records)           # deterministic order
    raw_path = os.path.join(args.out, 'rasters.raw')
    with open(raw_path, 'wb') as f:
        for h in order:
            f.write(records[h]['record'])
    orig = os.path.join(args.out, 'orig')
    res = subprocess.run([args.texkey, 'batch', raw_path, orig, '--png'], check=True, capture_output=True, text=True)
    lines = res.stdout.strip().split('\n')
    if len(lines) != len(order):
        raise SystemExit(f'texkey returned {len(lines)} lines for {len(order)} records')

    variants = collections.defaultdict(set)
    textures = []
    keys = {}
    collisions = []
    for index, (h, line) in enumerate(zip(order, lines)):
        i, key, w, hgt, fmt, zero, opaque, partial, pic, ikey = line.split()
        assert int(i) == index
        e = records[h]
        if key == '0000000000000000':
            raise SystemExit(f'record {index} ({list(e["names"])[:1]}) has no key')
        if key in keys:
            collisions.append((key, keys[key], index))
        keys[key] = index
        variants[e['image']].add(key)
        textures.append({'key': key, 'width': int(w), 'height': int(hgt), 'format': FORMAT_NAMES[int(fmt)], 'image': e['image'],
                         'names': sorted(e['names']), 'streams': sorted(e['streams']), 'uses': e['uses'],
                         'platforms': sorted(e['platforms']), 'class': classify(sorted(e['names'])[0]),
                         'alpha': {'zero': float(zero), 'opaque': float(opaque), 'partial': float(partial)},
                         'picture_hash': pic, 'image_key': ikey, 'raw_index': index})
    for t in textures:
        t['palette_variants'] = len(variants[t['image']])

    census = {
        'streams': per_stream,
        'rasters_in_streams': sum(per_stream.values()),
        'distinct_textures': len(textures),
        'distinct_images': len(variants),
        'key_collisions': collisions,
        'by_format': collections.Counter(t['format'] for t in textures),
        'by_size_format': {f'{t["width"]}x{t["height"]} {t["format"]}': 0 for t in textures},
        'palette_variants_histogram': dict(sorted(collections.Counter(len(v) for v in variants.values()).items())),
    }
    for t in textures:
        census['by_size_format'][f'{t["width"]}x{t["height"]} {t["format"]}'] += 1
    census['by_size_format'] = dict(sorted(census['by_size_format'].items(), key=lambda kv: -kv[1]))
    with open(os.path.join(args.out, 'catalog.json'), 'w') as f:
        json.dump({'textures': textures, 'census': census}, f, indent=1)
    print(json.dumps({k: v for k, v in census.items() if k not in ('streams', 'by_size_format')}, indent=1))
    return 0


if __name__ == '__main__':
    sys.exit(main())
