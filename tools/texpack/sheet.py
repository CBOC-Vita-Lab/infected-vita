#!/usr/bin/env python3
"""Texture-level comparison sheets: original (point-sampled) | original (bilinear, what the GPU does) | 2x | 4x.

    sheet.py rows    --extract E --up UP --rt RT --out sheet.png [selection] [--crop 256] [--zoom 1] [--bg checker|black|white|grey]
    sheet.py tile    --extract E --up UP --rt RT --out tile.png [selection]      the seam cross of a 2x2 tiling: bilinear original | 2x | 4x
    sheet.py contact --extract E --up UP --rt RT --out contact.png [selection] [--cell 128] [--scale 4]

Selection (all combine): --class ui|world|character|fx, --name REGEX (matches any name of the image), --ids ID,ID,
--min-side N, --max-side N, --seen (only images the renderer drew in the run-time harvest), --limit N, --sample SEED
(a seeded random pick of --limit images instead of the first ones), --engine ENGINE (use that engine's result, else the best finished).

Every panel is drawn at 4x of the source size (the original is enlarged by pixel replication or by a bilinear filter, the 2x result by
pixel replication, the 4x result as it is), so a 100 % crop of the 4x result is what the GPU holds at full size. Alpha pictures are
composited over --bg, so a halo (a fringe of the wrong colour at a cut-out edge) shows up; run it with black and with white to see both kinds.
"""
import argparse
import json
import os
import random
import re
import sys

import cv2
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import upscale  # noqa: E402
import upscale_lib as U  # noqa: E402


def background(h, w, kind):
    if kind == 'black':
        return np.zeros((h, w, 3), np.float32)
    if kind == 'white':
        return np.full((h, w, 3), 255, np.float32)
    if kind == 'grey':
        return np.full((h, w, 3), 128, np.float32)
    yy, xx = np.mgrid[0:h, 0:w]
    c = (((yy // 8) + (xx // 8)) % 2).astype(np.float32) * 60 + 100
    return np.dstack([c, c, c])


def composite(rgba, bg):
    a = rgba[..., 3:4].astype(np.float32) / 255.0
    base = background(rgba.shape[0], rgba.shape[1], bg)
    return np.clip(rgba[..., :3].astype(np.float32) * a + base * (1 - a), 0, 255).astype(np.uint8)


def resize_straight(rgba, size, interp):
    """Resize RGBA with the filter applied to straight (non-premultiplied) channels, as a GPU's GL_LINEAR does."""
    return cv2.resize(rgba, size, interpolation=interp)


def panels(entry, args, extract, up_dir):
    orig = U.to_rgba_png(os.path.join(extract, 'orig', entry['base_key'] + '.png'))
    h, w = orig.shape[:2]
    out = {'orig': np.repeat(np.repeat(orig, 4, 0), 4, 1),
           'bilinear': resize_straight(orig, (w * 4, h * 4), cv2.INTER_LINEAR)}
    engine = None
    for scale in (2, 4):
        if args.engine:
            path = upscale.result_paths(up_dir, entry, scale, args.engine)
            path = path if os.path.exists(path) else None
        else:
            path, engine = upscale.best_result(up_dir, entry, scale)
        if path is None:
            out[f'x{scale}'] = None
            continue
        img = U.to_rgba_png(path)
        if not args.engine and not args.raw:
            img, engine = upscale.load_final(extract, up_dir, entry, scale)
        out[f'x{scale}'] = np.repeat(np.repeat(img, 2, 0), 2, 1) if scale == 2 else img
    out['engine'] = engine or args.engine or '-'
    return out


def label(img, text, h=16):
    img = img.copy()
    cv2.rectangle(img, (0, 0), (img.shape[1], h), (0, 0, 0), -1)
    cv2.putText(img, text, (3, h - 4), cv2.FONT_HERSHEY_SIMPLEX, 0.4, (255, 255, 255), 1, cv2.LINE_AA)
    return img


def select(plan, args, observed_keys):
    sel = plan
    if args.klass:
        sel = [e for e in sel if e['class'] == args.klass]
    if args.name:
        rx = re.compile(args.name, re.I)
        sel = [e for e in sel if any(rx.search(n) for n in e['names'])]
    if args.ids:
        ids = set(args.ids.split(','))
        sel = [e for e in sel if e['image'] in ids]
    if args.min_side:
        sel = [e for e in sel if max(e['width'], e['height']) >= args.min_side]
    if args.max_side:
        sel = [e for e in sel if max(e['width'], e['height']) <= args.max_side]
    if args.seen:
        sel = [e for e in sel if e['image_key'] in observed_keys or any(k in observed_keys for k in e['keys'])]
    if args.sample is not None:
        rnd = random.Random(args.sample)
        sel = list(sel)
        rnd.shuffle(sel)
    if args.limit:
        sel = sel[:args.limit]
    return sel


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('mode', choices=['rows', 'contact', 'tile'])
    ap.add_argument('--extract', required=True)
    ap.add_argument('--up', required=True)
    ap.add_argument('--rt')
    ap.add_argument('--out', required=True)
    ap.add_argument('--class', dest='klass')
    ap.add_argument('--name')
    ap.add_argument('--ids')
    ap.add_argument('--min-side', type=int, default=0)
    ap.add_argument('--max-side', type=int, default=0)
    ap.add_argument('--seen', action='store_true')
    ap.add_argument('--limit', type=int, default=6)
    ap.add_argument('--sample', type=int)
    ap.add_argument('--engine', default='')
    ap.add_argument('--crop', type=int, default=256, help='rows: panel size limit in output pixels (the 4x picture)')
    ap.add_argument('--crop-at', default='', help='rows: x,y of the crop in SOURCE texels (default: centre)')
    ap.add_argument('--zoom', type=int, default=1, help='rows: extra pixel replication of every panel (small icons)')
    ap.add_argument('--compare-engines', default='', help='rows: two engine names (a,b): the 2x and 4x panels become the 4x results of a and b')
    ap.add_argument('--tone', default='', help='rows: sigma list s1,s2: panels become orig bilinear | 4x | 4x tone-matched s1 | s2')
    ap.add_argument('--raw', action='store_true', help='show the cached network output without the tone match')
    ap.add_argument('--bg', default='checker')
    ap.add_argument('--cell', type=int, default=128)
    ap.add_argument('--cols', type=int, default=8)
    args = ap.parse_args()
    plan = upscale.build_plan(args.extract, args.rt)
    import classify
    observed_keys = set(classify.load_observed(args.rt)) | classify.observed_image_keys(args.rt) if args.rt else set()
    sel = select(plan, args, observed_keys)
    if not sel:
        print('nothing selected')
        return 1
    rows = []
    cells = []
    for e in sel:
        p = panels(e, args, args.extract, args.up)
        h4, w4 = p['orig'].shape[:2]
        tag = f"{e['names'][0] if e['names'] else '?'} {e['width']}x{e['height']} {e['class']} {p['engine']} [{e['image'][:6]}]"
        if args.mode == 'contact':
            img = p['x4'] if p['x4'] is not None else p['bilinear']
            rgb = composite(img, args.bg)
            s = min(args.cell / rgb.shape[1], args.cell / rgb.shape[0])
            small = cv2.resize(rgb, (max(1, int(rgb.shape[1] * s)), max(1, int(rgb.shape[0] * s))), interpolation=cv2.INTER_AREA)
            cell = np.full((args.cell + 12, args.cell, 3), 24, np.uint8)
            cell[12:12 + small.shape[0], :small.shape[1]] = small
            cv2.putText(cell, f"{e['names'][0][:22] if e['names'] else '?'}", (1, 9), cv2.FONT_HERSHEY_SIMPLEX, 0.3, (255, 255, 255), 1, cv2.LINE_AA)
            cells.append(cell)
            continue
        if args.mode == 'tile':
            orig = U.to_rgba_png(os.path.join(args.extract, 'orig', e['base_key'] + '.png'))
            h0, w0 = orig.shape[:2]
            big = resize_straight(np.tile(orig, (3, 3, 1)), (w0 * 12, h0 * 12), cv2.INTER_LINEAR)
            half = min(96, h4, w4)

            def around_seam(tile):
                t = np.tile(tile, (2, 2, 1))
                return t[h4 - half:h4 + half, w4 - half:w4 + half]

            variants = [('orig bilinear (wrap)', around_seam(big[h4:h4 * 2, w4:w4 * 2]))]
            for key, text in (('x2', '2x'), ('x4', '4x')):
                img = p[key]
                if img is not None:
                    variants.append((text, around_seam(img)))
            pans = [label(composite(v, args.bg), text) for text, v in variants]
            gap = np.full((pans[0].shape[0], 4, 3), 24, np.uint8)
            row = np.hstack([x for pan in pans for x in (pan, gap)][:-1])
            header = np.full((16, row.shape[1], 3), 60, np.uint8)
            cv2.putText(header, tag + ' seam cross', (3, 12), cv2.FONT_HERSHEY_SIMPLEX, 0.4, (255, 255, 200), 1, cv2.LINE_AA)
            rows.append(np.vstack([header, row]))
            continue
        cw, ch = min(w4, args.crop), min(h4, args.crop)
        if args.crop_at:
            cx, cy = [int(v) * 4 for v in args.crop_at.split(',')]
        else:
            cx, cy = (w4 - cw) // 2, (h4 - ch) // 2
        cx, cy = max(0, min(cx, w4 - cw)), max(0, min(cy, h4 - ch))
        names = [('orig', 'orig point'), ('bilinear', 'orig bilinear'), ('x2', '2x'), ('x4', '4x')]
        if args.tone:
            s1, s2 = [float(v) for v in args.tone.split(',')]
            src = U.to_rgba_png(os.path.join(args.extract, 'orig', e['base_key'] + '.png'))
            base4 = p['x4']
            p['orig'], p['x2'] = base4, U.tone_match(src, base4, 4, s1)
            p['x4'] = U.tone_match(src, base4, 4, s2)
            names = [('orig', '4x as cached'), ('bilinear', 'orig bilinear'), ('x2', f'4x tone s={s1}'), ('x4', f'4x tone s={s2}')]
        if args.compare_engines:
            ea, eb = args.compare_engines.split(',')
            for k, eng in (('x2', ea), ('x4', eb)):
                path = upscale.result_paths(args.up, e, 4, eng)
                p[k] = U.to_rgba_png(path) if os.path.exists(path) else None
            names = [('orig', 'orig point'), ('bilinear', 'orig bilinear'), ('x2', ea + ' 4x'), ('x4', eb + ' 4x')]
        pans = []
        for key, text in names:
            img = p[key]
            if img is None:
                pan = np.full((ch, cw, 3), 40, np.uint8)
            else:
                pan = composite(img[cy:cy + ch, cx:cx + cw], args.bg)
            if args.zoom > 1:
                pan = np.repeat(np.repeat(pan, args.zoom, 0), args.zoom, 1)
            pans.append(label(pan, text))
        gap = np.full((pans[0].shape[0], 4, 3), 24, np.uint8)
        row = np.hstack([x for pan in pans for x in (pan, gap)][:-1])
        header = np.full((16, row.shape[1], 3), 60, np.uint8)
        cv2.putText(header, tag + f' crop {cx // 4},{cy // 4}', (3, 12), cv2.FONT_HERSHEY_SIMPLEX, 0.4, (255, 255, 200), 1, cv2.LINE_AA)
        rows.append(np.vstack([header, row]))
    if args.mode == 'contact':
        cols = args.cols
        while len(cells) % cols:
            cells.append(np.full_like(cells[0], 24))
        sheet = np.vstack([np.hstack(cells[i:i + cols]) for i in range(0, len(cells), cols)])
    else:
        width = max(r.shape[1] for r in rows)
        sheet = np.vstack([cv2.copyMakeBorder(r, 0, 4, 0, width - r.shape[1], cv2.BORDER_CONSTANT, value=(24, 24, 24)) for r in rows])
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    cv2.imwrite(args.out, sheet)
    print('wrote', args.out, sheet.shape, len(sel), 'images')
    for e in sel:
        print('  ', e['image'][:6], e['names'][0] if e['names'] else '?', e['width'], e['height'], e['class'], e['variants'])
    return 0


if __name__ == '__main__':
    sys.exit(main())
