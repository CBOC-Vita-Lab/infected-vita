#!/usr/bin/env python3
"""Comparison sheets of the upscaling engines on a representative sample of the disc's textures.

    eval_models.py --extract private/extract --models private/models --out private/eval [--engines a,b,c]

Every (texture, engine) result is cached as a PNG, so reruns only compute what is missing. One sheet per theme:
the first column is the original shown with nearest neighbour at the output scale, then one column per engine.
"""
import argparse
import json
import os
import sys
import time

import cv2
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import upscale_lib as U  # noqa: E402

SAMPLE = {
    'faces_clothes': ['avatar_c_head2', 'slipknot_0_head', 'avatar_c_body1', 'infected_a', 'civilian_b', 'avatar_f_hair3'],
    'buildings_street': ['flat_bricks_02', 'fifth_brick_window', 'street_013', 'CT_bldg10_windowA', 'CT_shop_windows_04', 'manholecover'],
    'signs_text': ['CT_sign_long_07', 'subway_sign_canalst_01', 'CT_sign_vert_06', 'signage_B02', 'TWS_SIGN', 'timesq_grim_sign_a'],
    'cutouts_alpha': ['subway_bars', 'prop_fireescape_ladder_01', 'heli_ladder', 'subway_windows', 'back_clouds_2', 'fx_blobshadow'],
    'fire_smoke': ['fxn_smoke_puff', 'fxr_molotov_fireball', 'fxn_blood_splash', 'fxr_fireball', 'fxn_fireball', 'fxn_white_light'],
    'vehicles': ['vehicle_sedan_side_b', 'vehicle_van_front', 'vehicle_police_side_b', 'vehicle_dtruck_front', 'vehicle_truck_02', 'vehicle_suv_back'],
    'hud_icons': ['icon_medal_complete', 'ui_burn', 'weapon_im_auto', 'lock_icon_2', 'civilians_icon', 'ballistic_weapon_pistol'],
    'fonts': ['ui_font_medium', 'ui_font_large_a', 'ui_font_small', 'font_numbers1', 'infected_logo', 'ui_font_body'],
}


def find(catalog, name):
    best = None
    for t in catalog['textures']:
        if t['names'][0] == name or name in t['names']:
            if best is None or t['uses'] > best['uses']:
                best = t
    return best


def checker(h, w, cell=8):
    y, x = np.mgrid[0:h, 0:w]
    c = (((y // cell) + (x // cell)) % 2) * 40 + 90
    return np.dstack([c, c, c]).astype(np.float32)


def flatten(rgba):
    a = rgba[..., 3:4].astype(np.float32) / 255.0
    bg = checker(*rgba.shape[:2])
    return (rgba[..., :3].astype(np.float32) * a + bg * (1 - a)).astype(np.uint8)


def label(img, text):
    cv2.rectangle(img, (0, 0), (img.shape[1], 14), (0, 0, 0), -1)
    cv2.putText(img, text, (3, 11), cv2.FONT_HERSHEY_SIMPLEX, 0.36, (255, 255, 255), 1, cv2.LINE_AA)
    return img


def make_engine(name, models_dir):
    if name == 'nearest':
        return lambda rgba, s, **k: U.nearest(rgba, s)
    if name == 'lanczos':
        return lambda rgba, s, **k: U.lanczos(rgba, s)
    if name == 'scale2x':
        return lambda rgba, s, **k: U.scale2x(rgba, s)
    if name == 'sharpbilinear':
        return lambda rgba, s, **k: U.sharp_bilinear(rgba, s)
    eng = U.AiEngine(name, models_dir)
    return lambda rgba, s, wrap=True, **k: eng.upscale(rgba, s, wrap=wrap)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--extract', required=True)
    ap.add_argument('--models', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--engines', default='nearest,lanczos,scale2x,general-x4v3,animevideov3,hdcube4-compact,x4plus')
    ap.add_argument('--themes', default='')
    ap.add_argument('--scale', type=int, default=4)
    ap.add_argument('--cell', type=int, default=256)
    ap.add_argument('--max-ai-px', type=int, default=16384, help='skip the slow networks above this many source pixels')
    args = ap.parse_args()
    catalog = json.load(open(os.path.join(args.extract, 'catalog.json')))
    os.makedirs(args.out, exist_ok=True)
    engines = args.engines.split(',')
    slow = {'x4plus', 'gtav'}
    timings = {}
    themes = args.themes.split(',') if args.themes else list(SAMPLE)
    for theme in themes:
        rows = []
        for name in SAMPLE[theme]:
            t = find(catalog, name)
            if t is None:
                print('missing', name)
                continue
            rgba = U.to_rgba_png(os.path.join(args.extract, 'orig', t['key'] + '.png'))
            h, w = rgba.shape[:2]
            cells = []
            for en in engines:
                cache = os.path.join(args.out, 'cache', f"{t['key']}_{en}_{args.scale}.png")
                if os.path.exists(cache):
                    up = U.to_rgba_png(cache)
                elif en in slow and h * w > args.max_ai_px:
                    up = None
                else:
                    os.makedirs(os.path.dirname(cache), exist_ok=True)
                    fn = make_engine(en, args.models) if en not in globals().get('_ENG', {}) else None
                    globals().setdefault('_ENG', {})
                    if en not in _ENG:
                        _ENG[en] = make_engine(en, args.models)
                    t0 = time.time()
                    up = _ENG[en](rgba, args.scale, wrap=not name.lower().startswith(('ui_', 'font', 'icon')))
                    dt = time.time() - t0
                    timings.setdefault(en, []).append((h * w, dt))
                    from PIL import Image
                    Image.fromarray(up).save(cache)
                    print(f'{theme} {name} {en} {w}x{h} {dt:.1f}s', flush=True)
                if up is None:
                    cells.append(np.full((args.cell, args.cell, 3), 30, np.uint8))
                    continue
                img = flatten(up)
                # 100% crop around the centre
                ch, cw = img.shape[:2]
                y0 = max(0, (ch - args.cell) // 2)
                x0 = max(0, (cw - args.cell) // 2)
                crop = img[y0:y0 + args.cell, x0:x0 + args.cell]
                pad = np.full((args.cell, args.cell, 3), 20, np.uint8)
                pad[:crop.shape[0], :crop.shape[1]] = crop
                cells.append(label(pad, f'{name} {w}x{h} | {en}'))
            rows.append(np.hstack(cells))
        if rows:
            sheet = np.vstack(rows)
            from PIL import Image
            path = os.path.join(args.out, f'sheet_{theme}_x{args.scale}.png')
            Image.fromarray(sheet).save(path)
            print('wrote', path, sheet.shape)
    summary = {en: {'textures': len(v), 'px': sum(p for p, _ in v), 'seconds': sum(d for _, d in v)} for en, v in timings.items()}
    json.dump(summary, open(os.path.join(args.out, 'timings_%d.json' % int(time.time())), 'w'), indent=1)
    print(json.dumps(summary, indent=1))


if __name__ == '__main__':
    main()
