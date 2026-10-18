#!/usr/bin/env python3
"""Upscale every distinct image of the disc, resumably.

    upscale.py plan   --extract E [--rt RT]                       class and engine of every image, totals
    upscale.py run    --extract E --models M --out OUT [--limit N] [--deadline SECONDS] [--threads 2]
    upscale.py status --extract E --out OUT

An *image* is a texel buffer; its palette variants (CLUT animation frames, recoloured clothes) are made from one
upscale of the most used variant, in pack.py. Results are cached per (image, engine, settings version), so a run
that is stopped (or killed) resumes where it was; finished work is never repeated.

Engines by class (see eval_models.py and the sheets in private/eval for the evidence):
    character  RealESRGAN_x4plus (RRDB): the most natural faces and cloth, no invented contrast
    world, ui  4xHDCube4Compact (a Real-ESRGAN Compact trained on GameCube/Wii texture dumps: removes dither,
               banding and palette errors): fast and faithful
    fx         bicubic with a bled colour: soft sprites gain nothing from a network (hair strands of the characters too)
After a network the picture is tone matched to its source (load_final): the networks repaint, shifting the mean colour of areas by a
few levels and losing pure black and white; tone_match restores the source's low frequencies and keeps the network's detail.
    ui tiny    Scale2x (pixel-exact edges for 16x16 icons and hatch patterns)
Alpha is always separate (upscale_lib.upscale_alpha): cut-outs stay crisp, soft glows stay soft, and the colour
under transparent texels is bled from the visible ones so no halo can appear.
"""
import argparse
import collections
import json
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import classify  # noqa: E402
import upscale_lib as U  # noqa: E402

SETTINGS_VERSION = 4          # bump when pad/alpha/engine behaviour changes: the cache is keyed by it
PRIORITY_STREAMS = ['Global', 'MainMenuLevel', 'SplashVideosLevel', 'Mission_1A', 'Mission_2A', 'Mission_2B',
                    'Mission_3A', 'Mission_3B']
# Engines in order of preference. The fast pass (the default) runs the LAST engine of each list, so every texture has a
# result after a short run; the best pass runs the first one where it differs and pack.py takes it when it exists.
ENGINE_BY_CLASS = {'character': ['x4plus', 'hdcube4-compact'], 'world': ['hdcube4-compact'], 'ui': ['hdcube4-compact'],
                   'fx': ['bicubic']}
# the networks repaint: after them the picture gets the colour and large-scale brightness of its source back (upscale_lib.tone_match)
NETWORK_ENGINES = ('hdcube4-compact', 'x4plus')
TONE_SIGMA = 1.5
STRIPE_LIMIT = 3.0
TINY_UI_PIXELS = 1024         # up to 32x32: pixel-exact scaler


def choose_engines(cls, width, height, overrides=None, names=(), stripes=0.0):
    if min(width, height) <= 4:
        return ['nearest']    # palette carriers and strips (2x2, 4x4): nothing for a network to see
    if overrides and cls in overrides:
        return [overrides[cls]]
    if cls == 'fx' and stripes > STRIPE_LIMIT:
        return ['nearest']    # interlaced sprites (vomit trail, scuttle spark): alternate rows are the picture
    if cls == 'character' and any('hair' in n.lower() for n in names):
        return ['bicubic']    # strand textures with soft alpha: a network invents thin bright lines, plain bicubic keeps them as they are
    if cls == 'ui' and width * height <= TINY_UI_PIXELS:
        return ['scale2x']
    return list(ENGINE_BY_CLASS[cls])


def build_plan(extract, rt=None):
    cat = json.load(open(os.path.join(extract, 'catalog.json')))
    observed = classify.load_observed(rt) if rt else {}
    groups = collections.OrderedDict()
    for t in cat['textures']:
        groups.setdefault(t['image'], []).append(t)
    plan = []
    for image, ts in groups.items():
        base = max(ts, key=lambda t: (t['uses'], t['key']))
        names = sorted({n for t in ts for n in t['names']})
        keys = [t['key'] for t in ts]
        cls = classify.image_class(names, keys, observed)
        stripes = 0.0
        if cls == 'fx' and min(base['width'], base['height']) > 4:
            stripes = U.stripe_score(U.to_rgba_png(os.path.join(extract, 'orig', base['key'] + '.png')))
        streams = sorted({s for t in ts for s in t['streams']})
        rank = min([PRIORITY_STREAMS.index(s) for s in streams if s in PRIORITY_STREAMS] or [len(PRIORITY_STREAMS)])
        plan.append({'image': image, 'base_key': base['key'], 'keys': keys, 'names': names[:4], 'width': base['width'],
                     'height': base['height'], 'class': cls, 'engines': choose_engines(cls, base['width'], base['height'], None, names, stripes),
                     'variants': len(ts), 'streams': len(streams), 'rank': rank, 'format': base['format'],
                     'image_key': base.get('image_key'), 'stripes': round(stripes, 2)})
    plan.sort(key=lambda p: (p['rank'], p['width'] * p['height'], p['image']))
    return plan


def result_paths(out, entry, scale, engine=None):
    tag = f"{engine or entry['engines'][-1]}-v{SETTINGS_VERSION}"
    return os.path.join(out, f'x{scale}', f"{entry['image']}_{tag}.png")


def best_result(out, entry, scale):
    """The path of the best finished result for the image at this scale (engines in order of preference), or None."""
    for engine in entry['engines']:
        path = result_paths(out, entry, scale, engine)
        if os.path.exists(path):
            return path, engine
    return None, None


def load_final(extract, out, entry, scale):
    """(picture, engine) of the best finished result with the tone match applied (what the pack and the sheets use), or (None, None)."""
    path, engine = best_result(out, entry, scale)
    if path is None:
        return None, None
    img = U.to_rgba_png(path)
    if engine in NETWORK_ENGINES:
        src = U.to_rgba_png(os.path.join(extract, 'orig', entry['base_key'] + '.png'))
        img = U.tone_match(src, img, scale, TONE_SIGMA)
    return img, engine


def process(entry, extract, models_dir, out, engines, threads, eng):
    from PIL import Image
    rgba = U.to_rgba_png(os.path.join(extract, 'orig', entry['base_key'] + '.png'))
    h, w = rgba.shape[:2]
    wrap = classify.wrap_hint(entry['class'])
    results = {}
    if eng in ('lanczos', 'bicubic', 'scale2x', 'nearest'):
        for s in (2, 4):
            results[s] = {'lanczos': U.lanczos, 'bicubic': U.bicubic, 'scale2x': U.scale2x, 'nearest': U.nearest}[eng](rgba, s)
    else:
        if eng not in engines:
            engines[eng] = U.AiEngine(eng, models_dir, threads=threads)
        up4 = engines[eng].upscale(rgba, 4, wrap=wrap, pad=min(16, max(4, min(w, h))))
        results[4] = up4
        # 2x: the 4x picture averaged down (a supersampled 2x, sharper than a 2x network pass)
        import cv2
        kind = U.alpha_kind(rgba)
        rgb2 = cv2.resize(up4[..., :3], (w * 2, h * 2), interpolation=cv2.INTER_AREA)
        results[2] = np.dstack([rgb2, U.upscale_alpha(rgba[..., 3], 2, kind)])
    for s, img in results.items():
        path = result_paths(out, entry, s, eng)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        tmp = path + '.tmp.png'
        Image.fromarray(img).save(tmp)
        os.replace(tmp, path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('command', choices=['plan', 'run', 'status', 'clean'])
    ap.add_argument('--extract', required=True)
    ap.add_argument('--models')
    ap.add_argument('--out')
    ap.add_argument('--rt', help='run-time harvest directory (INFECTED_TEX_DUMP outputs) for the draw kinds')
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--deadline', type=float, default=0.0, help='stop starting new images after this many seconds')
    ap.add_argument('--threads', type=int, default=2)
    ap.add_argument('--only', default='', help='comma separated classes')
    ap.add_argument('--delete', action='store_true', help='clean: delete the stale results')
    ap.add_argument('--engine', default='', help='class=engine overrides, comma separated')
    ap.add_argument('--pass', dest='which', choices=['fast', 'best'], default='fast',
                    help='fast: the quickest engine of every class; best: the preferred engine where it is a different one')
    args = ap.parse_args()
    plan = build_plan(args.extract, args.rt)
    observed = classify.load_observed(args.rt) if args.rt else {}
    if args.engine:
        ov = dict(kv.split('=') for kv in args.engine.split(','))
        for e in plan:
            e['engines'] = choose_engines(e['class'], e['width'], e['height'], ov, e['names'], e.get('stripes', 0.0))
    if args.only:
        keep = set(args.only.split(','))
        plan = [e for e in plan if e['class'] in keep]
    if args.command == 'plan':
        tot = collections.defaultdict(lambda: [0, 0])
        for e in plan:
            tot[(e['class'], '+'.join(e['engines']))][0] += 1
            tot[(e['class'], '+'.join(e['engines']))][1] += e['width'] * e['height']
        for (c, en), (n, px) in sorted(tot.items()):
            print(f'{c:10s} {en:34s} images {n:5d} pixels {px:9d}')
        print('images', len(plan), 'pixels', sum(e['width'] * e['height'] for e in plan))
        return 0
    out = args.out
    if args.command == 'clean':
        # results that no plan entry refers to any more (a class or engine policy changed): listed, deleted with --delete
        keep = {os.path.basename(result_paths(out, e, sc, en)) for e in build_plan(args.extract, args.rt) for en in e['engines'] for sc in (2, 4)}
        stale = []
        for sub in ('x2', 'x4'):
            d = os.path.join(out, sub)
            stale += [os.path.join(d, f) for f in sorted(os.listdir(d)) if f.endswith('.png') and f not in keep] if os.path.isdir(d) else []
        size = sum(os.path.getsize(f) for f in stale)
        print(f'{len(stale)} stale results, {size / 1e6:.1f} MB' + (' (deleted)' if args.delete else ' (use --delete)'))
        if args.delete:
            for f in stale:
                os.remove(f)
        return 0

    def engine_of(e):
        return e['engines'][-1] if args.which == 'fast' else e['engines'][0]

    def finished(e):
        eng = engine_of(e)
        return os.path.exists(result_paths(out, e, 4, eng)) and os.path.exists(result_paths(out, e, 2, eng))

    work = plan if args.which == 'fast' else [e for e in plan if len(e['engines']) > 1]
    if args.which == 'best':
        # the slow network goes first to what was actually seen on screen (run-time harvest), then to the rest
        seen_images = classify.observed_image_keys(args.rt)
        work.sort(key=lambda e: (not (e['image_key'] in seen_images or any(k in observed for k in e['keys'])), e['rank'],
                                 e['width'] * e['height'], e['image']))
    done = [e for e in work if finished(e)]
    todo = [e for e in work if not finished(e)]
    if args.command == 'status':
        print(f'pass {args.which}: {len(done)} of {len(work)} images done, {sum(e["width"] * e["height"] for e in done)} of '
              f'{sum(e["width"] * e["height"] for e in work)} pixels')
        return 0
    try:
        os.nice(10)
    except OSError:
        pass
    os.makedirs(out, exist_ok=True)
    log = open(os.path.join(out, 'progress.jsonl'), 'a')
    engines = {}
    start = time.time()
    count = 0
    for e in todo:
        if args.deadline and time.time() - start > args.deadline:
            print('deadline reached')
            break
        if args.limit and count >= args.limit:
            break
        t0 = time.time()
        eng = engine_of(e)
        process(e, args.extract, args.models, out, engines, args.threads, eng)
        dt = time.time() - t0
        count += 1
        log.write(json.dumps({'image': e['image'], 'engine': eng, 'class': e['class'], 'w': e['width'],
                              'h': e['height'], 'seconds': round(dt, 3), 'settings': SETTINGS_VERSION}) + '\n')
        log.flush()
        if count % 25 == 0:
            print(f'{count} done, {len(todo) - count} left, {time.time() - start:.0f}s', flush=True)
    print(f'finished {count} images in {time.time() - start:.0f}s; {len(todo) - count} left')
    return 0


if __name__ == '__main__':
    sys.exit(main())
