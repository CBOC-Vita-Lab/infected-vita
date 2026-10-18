#!/usr/bin/env python3
"""Does a light de-dither before the network help? A measurement with a known answer.

Ground truth: the disc's own 128x128 and 256x128 CLUT8 pictures (256 colours, no visible dither). Each is shrunk 2x with an
area filter, quantised to 16 colours with Floyd-Steinberg dithering (what a CLUT4 texture of this game looks like) and
upscaled back 2x by: Lanczos, the network alone, a 3x3 median + network, a Gaussian + network. PSNR and SSIM against
the ground truth say which route recovers the picture best. (The network works at 4x; the result is averaged down to 2x.)
"""
import argparse
import json
import os
import sys

import cv2
import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import upscale_lib as U  # noqa: E402


def psnr(a, b):
    d = a.astype(np.float64) - b.astype(np.float64)
    return 99.0 if (d * d).mean() == 0 else 10 * np.log10(255.0 ** 2 / (d * d).mean())


def ssim(a, b):
    a = cv2.cvtColor(a, cv2.COLOR_RGB2GRAY).astype(np.float64)
    b = cv2.cvtColor(b, cv2.COLOR_RGB2GRAY).astype(np.float64)
    c1, c2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    k = (7, 7)
    ma, mb = cv2.blur(a, k), cv2.blur(b, k)
    saa, sbb, sab = cv2.blur(a * a, k) - ma * ma, cv2.blur(b * b, k) - mb * mb, cv2.blur(a * b, k) - ma * mb
    s = ((2 * ma * mb + c1) * (2 * sab + c2)) / ((ma * ma + mb * mb + c1) * (saa + sbb + c2))
    return float(s.mean())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--extract', required=True)
    ap.add_argument('--models', required=True)
    ap.add_argument('--count', type=int, default=24)
    args = ap.parse_args()
    cat = json.load(open(os.path.join(args.extract, 'catalog.json')))
    pics, seen = [], set()
    for t in cat['textures']:
        n = t['names'][0].lower()
        if t['image'] in seen or t['format'] != 'clut8' or t['width'] < 128 or t['alpha']['opaque'] < 99:
            continue
        if n.startswith(('ui_', 'fx', 'icon', 'aa_', 'avatar', 'infected', 'civilian', 'album')):
            continue
        seen.add(t['image'])
        pics.append(t)
    pics = pics[::max(1, len(pics) // args.count)][:args.count]
    eng = U.AiEngine('hdcube4-compact', args.models)
    out = {k: [[], []] for k in ('nearest', 'lanczos', 'ai', 'median3+ai', 'gauss0.6+ai', 'dither only (no scaling)')}
    for t in pics:
        gt = U.to_rgba_png(os.path.join(args.extract, 'orig', t['key'] + '.png'))[..., :3]
        h, w = gt.shape[:2]
        small = cv2.resize(gt, (w // 2, h // 2), interpolation=cv2.INTER_AREA)
        q = np.array(Image.fromarray(small).quantize(colors=16, dither=Image.FLOYDSTEINBERG).convert('RGB'))
        cands = {
            'nearest': np.repeat(np.repeat(q, 2, 0), 2, 1),
            'lanczos': cv2.resize(q, (w, h), interpolation=cv2.INTER_LANCZOS4),
        }
        for name, src in (('ai', q), ('median3+ai', cv2.medianBlur(q, 3)), ('gauss0.6+ai', cv2.GaussianBlur(q, (0, 0), 0.6))):
            up = eng.run_rgb(src, 16, True)
            cands[name] = cv2.resize(up, (w, h), interpolation=cv2.INTER_AREA)
        cands['dither only (no scaling)'] = cv2.resize(q, (w, h), interpolation=cv2.INTER_NEAREST)
        for name, img in cands.items():
            out[name][0].append(psnr(gt, img))
            out[name][1].append(ssim(gt, img))
    print(f'{len(pics)} pictures, 16-colour Floyd-Steinberg dither at half size, restored to full size')
    print(f'{"route":28s} {"PSNR dB":>8s} {"SSIM":>7s}')
    for k, (p, s) in out.items():
        print(f'{k:28s} {np.mean(p):8.2f} {np.mean(s):7.4f}')


if __name__ == '__main__':
    main()
