#!/usr/bin/env python3
"""Objective checks of every finished upscale against its source, to find the outliers worth looking at.

    quality_scan.py --extract E --up UP --rt RT --csv private/compare/scan.csv [--limit N] [--scale 4]

Per image (the best finished result of the plan at the scale): the 4x picture is averaged back down to the source size and compared
with the source over the visible texels (alpha-weighted):
    psnr     fidelity of the content (high = faithful; a network that repaints scores lower than a plain filter)
    dmean    largest per-channel difference of the mean colour, in 0..255 (a colour shift: should be < 3)
    dsat     share of channel values at 0 or 255 in the result minus the same share in the source (contrast boost / clipping: ringing)
    dalpha   mean absolute alpha difference after averaging down, 0..255
    overshoot  share of result texels that leave the source's local 3x3 min..max range by more than 24 levels after averaging down (ringing / halos)
and prints the per class and engine summary and the worst few of each measure.
"""
import argparse
import collections
import csv
import os
import sys

import cv2
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import upscale  # noqa: E402
import upscale_lib as U  # noqa: E402


def over_grey(rgba):
    """Appearance of an RGBA picture over mid grey (hidden colour under zero alpha does not count)."""
    a = rgba[..., 3:4].astype(np.float32) / 255.0
    return rgba[..., :3].astype(np.float32) * a + 128.0 * (1.0 - a)


def measure(orig, up, scale):
    h, w = orig.shape[:2]
    # average down in premultiplied space (what a GPU's minification of the upscaled texture shows), then compare appearance over grey
    pm = up.astype(np.float32)
    pm[..., :3] *= pm[..., 3:4] / 255.0
    down = cv2.resize(pm, (w, h), interpolation=cv2.INTER_AREA)
    da = down[..., 3:4] / 255.0
    d = down[..., :3] + 128.0 * (1.0 - da)
    o = over_grey(orig)
    mse = float(((o - d) ** 2).mean())
    psnr = 99.0 if mse < 1e-6 else 10 * np.log10(255.0 ** 2 / mse)
    a = orig[..., 3].astype(np.float32) / 255.0
    vis = a > 0.02
    # colour shift: mean colour of the visible content, premultiplied sums over the alpha sum
    asum = max(float(a.sum()), 1e-6)
    mo = (orig[..., :3].astype(np.float32) * a[..., None]).sum((0, 1)) / asum
    md = down[..., :3].sum((0, 1)) / max(float(da.sum()), 1e-6)
    dmean = float(np.abs(mo - md).max())
    sat = lambda im, m: float(((im[m] <= 0) | (im[m] >= 255)).mean()) if m.any() else 0.0
    upvis = cv2.resize(vis.astype(np.uint8), (w * scale, h * scale), interpolation=cv2.INTER_NEAREST) > 0
    dsat = sat(up[..., :3], upvis) - sat(orig[..., :3], vis)
    dalpha = float(np.abs(down[..., 3] - orig[..., 3].astype(np.float32)).mean())
    o8 = np.clip(o, 0, 255).astype(np.uint8)
    lo = cv2.erode(o8, np.ones((3, 3), np.uint8)).astype(np.float32)
    hi = cv2.dilate(o8, np.ones((3, 3), np.uint8)).astype(np.float32)
    out = ((d < lo - 24) | (d > hi + 24)).any(axis=2)
    overshoot = float(out.mean())
    return psnr, dmean, dsat, dalpha, overshoot


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--extract', required=True)
    ap.add_argument('--up', required=True)
    ap.add_argument('--rt')
    ap.add_argument('--csv', required=True)
    ap.add_argument('--scale', type=int, default=4)
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--raw', action='store_true', help='measure the cached network output, without the tone match')
    ap.add_argument('--engine', default='', help='measure this engine\'s result instead of the best finished one')
    args = ap.parse_args()
    os.nice(10)
    plan = upscale.build_plan(args.extract, args.rt)
    if args.limit:
        plan = plan[:args.limit]
    rows = []
    for e in plan:
        if args.engine:
            path = upscale.result_paths(args.up, e, args.scale, args.engine)
            path, engine = (path, args.engine) if os.path.exists(path) else (None, None)
        else:
            path, engine = upscale.best_result(args.up, e, args.scale)
        if path is None:
            continue
        orig = U.to_rgba_png(os.path.join(args.extract, 'orig', e['base_key'] + '.png'))
        up = U.to_rgba_png(path)
        if not args.engine and not args.raw:
            up, engine = upscale.load_final(args.extract, args.up, e, args.scale)
        psnr, dmean, dsat, dalpha, over = measure(orig, up, args.scale)
        rows.append(dict(image=e['image'], name=e['names'][0] if e['names'] else '', cls=e['class'], engine=engine, w=e['width'],
                         h=e['height'], psnr=round(psnr, 2), dmean=round(dmean, 2), dsat=round(dsat, 4), dalpha=round(dalpha, 2),
                         overshoot=round(over, 4)))
    with open(args.csv, 'w', newline='') as f:
        wr = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        wr.writeheader()
        wr.writerows(rows)
    groups = collections.defaultdict(list)
    for r in rows:
        groups[(r['cls'], r['engine'])].append(r)
    print(f'{"class":10s} {"engine":16s} {"n":>5s} {"psnr":>6s} {"p10psnr":>8s} {"dmean>3":>8s} {"dsat":>7s} {"dalpha":>7s} {"over":>7s} {"over>2%":>8s}')
    for (c, en), rs in sorted(groups.items()):
        ps = np.array([r['psnr'] for r in rs])
        print(f'{c:10s} {en:16s} {len(rs):5d} {ps.mean():6.2f} {np.percentile(ps, 10):8.2f} {sum(r["dmean"] > 3 for r in rs):8d} '
              f'{np.mean([r["dsat"] for r in rs]):7.4f} {np.mean([r["dalpha"] for r in rs]):7.2f} {np.mean([r["overshoot"] for r in rs]):7.4f} '
              f'{sum(r["overshoot"] > 0.02 for r in rs):8d}')
    for key, rev in (('psnr', False), ('dmean', True), ('overshoot', True), ('dalpha', True)):
        worst = sorted((r for r in rows if min(r['w'], r['h']) > 4), key=lambda r: r[key], reverse=rev)[:8]
        print(f'worst {key}:', ', '.join(f"{r['name']}({r['w']}x{r['h']},{r['cls']},{r[key]})" for r in worst))
    return 0


if __name__ == '__main__':
    sys.exit(main())
