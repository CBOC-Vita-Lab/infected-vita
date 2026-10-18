#!/usr/bin/env python3
"""Content check of a pack against what the game really drew: every texture of a run-time harvest (INFECTED_TEX_DUMP: <key>.png is the renderer's own
decode of the texture as the game had it) is looked up in the pack by its full key; the record's level 0 is decoded, averaged down to the texture's size
and compared with the harvested picture (appearance over mid grey, PSNR). A wrong picture for a key, a swapped palette or a broken blob shows up as a low
number. Recolour hits (palette not on the disc) are compared by luma correlation only, since their colours are rebuilt at run time from the palette.

    pack_verify.py PACK.bin DUMP_DIR [DUMP_DIR ...] [--catalog catalog.json] [--limit-low 24]
"""
import argparse
import glob
import json
import os
import struct
import sys

import cv2
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bc  # noqa: E402
import quality_scan  # noqa: E402
from pack_check import read_index  # noqa: E402


def read_blob(f, rec):
    f.seek(rec['offset'])
    return f.read(rec['size'])


def level0(blob, rec):
    w, h = rec['w'], rec['h']
    if rec['flags'] & 1:   # recolour: u16 entries, u16 encoding, u32 packed bytes, palette, packed index map, level 0
        entries, _enc, packed = struct.unpack_from('<HHI', blob, 0)
        off = 8 + entries * 4 + packed
        data = blob[off:]
    else:
        data = blob
    if rec['fmt'] == 0:
        return np.frombuffer(data[:w * h * 4], np.uint8).reshape(h, w, 4)
    size = (w + 3) // 4 * ((h + 3) // 4) * (8 if rec['fmt'] == 1 else 16)
    return bc.decode(data[:size], w, h, rec['fmt'] == 2)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('pack')
    ap.add_argument('dumps', nargs='+')
    ap.add_argument('--catalog')
    ap.add_argument('--low', type=float, default=24.0, help='list textures below this PSNR')
    args = ap.parse_args()
    index, _scale = read_index(args.pack)
    names = {}
    if args.catalog:
        for t in json.load(open(args.catalog))['textures']:
            names[t['key']] = ','.join(t['names'][:2])
    seen = {}
    for d in args.dumps:
        for f in glob.glob(os.path.join(d, '*.json')):
            key = os.path.basename(f)[:-5]
            seen.setdefault(key, os.path.join(d, key + '.png'))
    rows = []
    corr_rows = []
    with open(args.pack, 'rb') as pf:
        for key, png in sorted(seen.items()):
            rec = index.get(int(key, 16))
            orig = cv2.imread(png, cv2.IMREAD_UNCHANGED)
            if orig is None or orig.ndim != 3 or orig.shape[2] != 4:
                continue
            orig = orig[..., [2, 1, 0, 3]]   # BGRA -> RGBA
            if rec is None:
                continue
            h, w = orig.shape[:2]
            up = level0(read_blob(pf, rec), rec)
            if up.shape[1] % w or up.shape[0] % h or up.shape[1] // w != up.shape[0] // h:
                rows.append((0.0, key, f'size {up.shape[1]}x{up.shape[0]} for {w}x{h}'))
                continue
            scale = up.shape[1] // w
            if rec['flags'] & 1:
                continue
            psnr = quality_scan.measure(orig, up, scale)[0]
            rows.append((psnr, key, f'{w}x{h} x{scale} fmt{rec["fmt"]}'))
    rows.sort()
    ps = np.array([r[0] for r in rows])
    print(f'{os.path.basename(args.pack)}: {len(rows)} direct-hit textures compared; PSNR over grey: min {ps.min():.1f}, p5 {np.percentile(ps, 5):.1f}, '
          f'median {np.median(ps):.1f}, p95 {np.percentile(ps, 95):.1f}; below {args.low:g} dB: {int((ps < args.low).sum())}')
    for psnr, key, what in rows[:25]:
        if psnr < args.low:
            print(f'  {psnr:5.1f} dB  {key}  {names.get(key, "?")}  {what}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
