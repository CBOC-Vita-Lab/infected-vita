#!/usr/bin/env python3
"""Replays recorded GE dumps with the HD pack off / 2x / 4x and builds labelled side-by-side sheets with 100% crops.

    compare.py --replay build/bin/ge_replay --dumps /path/to/dumps --packs private/packs --out private/compare scenes.json

scenes.json: [{"name": "street", "dump": "run9", "flip": 4500, "scale": 2,
               "crops": [[x, y, w, h], ...]}]      (crop boxes in output pixels at `scale`; default: none, just the frame)

For every scene a sheet <name>.png: row 1 the whole frame (off | 2x | 4x, shrunk to fit), then one row per crop at 1:1
pixels. Replays run once per (dump, mode, scale) over all the flips asked for, so the texture cache behaves as in a run.
The ge_replay stats line of each replay (hits, misses, resident bytes) is written to <name>.txt.
"""
import argparse
import collections
import json
import os
import subprocess
import sys

import cv2
import numpy as np

MODES = ['off', '2x', '4x']


def replay(exe, dump, flips, mode, scale, pack_dir, out_pattern, budget_mb):
    env = dict(os.environ, EGL_PLATFORM='surfaceless', LIBGL_ALWAYS_SOFTWARE='1')
    cmd = ['nice', '-n', '10', exe, dump, '--flips', ','.join(str(f) for f in flips), '--scale', str(scale),
           '--png', out_pattern, '--stats', '--hd', mode, '--pack-dir', pack_dir, '--hd-budget-mb', str(budget_mb)]
    res = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=3000)
    lines = [l.strip() for l in res.stdout.split('\n') + res.stderr.split('\n') if 'hd textures' in l or '[hdtex]' in l or 'texture working set' in l]
    return lines


def label(img, text):
    img = img.copy()
    cv2.rectangle(img, (0, 0), (img.shape[1], 18), (0, 0, 0), -1)
    cv2.putText(img, text, (4, 13), cv2.FONT_HERSHEY_SIMPLEX, 0.45, (255, 255, 255), 1, cv2.LINE_AA)
    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('scenes')
    ap.add_argument('--replay', required=True)
    ap.add_argument('--dumps', required=True)
    ap.add_argument('--packs', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--budget-mb', type=int, default=256)
    ap.add_argument('--modes', default='off,2x,4x')
    ap.add_argument('--full', action='store_true', help='also write <name>_full.png: whole frames, first and last mode side by side')
    args = ap.parse_args()
    scenes = json.load(open(args.scenes))
    modes = args.modes.split(',')
    os.makedirs(os.path.join(args.out, 'frames'), exist_ok=True)
    groups = collections.defaultdict(list)
    for s in scenes:
        groups[(s['dump'], s.get('scale', 2))].append(s['flip'])
    summaries = {}
    for (dump, scale), flips in groups.items():
        flips = sorted(set(flips))
        for mode in modes:
            pattern = os.path.join(args.out, 'frames', f'{dump}_{mode}_s{scale}_{{flip}}.png')
            todo = [f for f in flips if not os.path.exists(pattern.replace('{flip}', '%06d' % f))]
            if todo:
                lines = replay(args.replay, os.path.join(args.dumps, dump), flips, mode, scale, args.packs, pattern, args.budget_mb)
                summaries[(dump, mode, scale)] = lines
                print(dump, mode, scale, *lines, sep='\n  ', flush=True)
    for s in scenes:
        scale = s.get('scale', 2)
        frames = []
        for mode in modes:
            path = os.path.join(args.out, 'frames', f"{s['dump']}_{mode}_s{scale}_{s['flip']:06d}.png")
            img = cv2.imread(path, cv2.IMREAD_COLOR)
            if img is None:
                print('missing', path)
                frames.append(None)
            else:
                frames.append(img)
        ref = next((f for f in frames if f is not None), None)
        if ref is None:
            continue
        frames = [f if f is not None else np.zeros_like(ref) for f in frames]
        rows = []
        width = 1536
        cell = width // len(frames)
        small = [cv2.resize(f, (cell, int(f.shape[0] * cell / f.shape[1])), interpolation=cv2.INTER_AREA) for f in frames]
        rows.append(np.hstack([label(im, f"{s['name']} {m}") for im, m in zip(small, modes)]))
        zoom = s.get('zoom', 1)
        for x, y, w, h in s.get('crops', []):
            w = min(w, cell // zoom)
            crops = [f[y:y + h, x:x + w] for f in frames]
            if zoom > 1:
                crops = [cv2.resize(c, None, fx=zoom, fy=zoom, interpolation=cv2.INTER_NEAREST) for c in crops]
            pad = [cv2.copyMakeBorder(c, 0, 0, 0, cell - c.shape[1], cv2.BORDER_CONSTANT, value=(30, 30, 30)) for c in crops]
            rows.append(np.hstack([label(c, f'crop {x},{y} {w}x{h} x{zoom} {m}') for c, m in zip(pad, modes)]))
        hmax = max(r.shape[0] for r in rows)
        sheet = np.vstack([cv2.copyMakeBorder(r, 0, 0, 0, max(0, width - r.shape[1]), cv2.BORDER_CONSTANT, value=(30, 30, 30))[:, :width] for r in rows])
        out = os.path.join(args.out, f"{s['name']}.png")
        cv2.imwrite(out, sheet)
        print('wrote', out, sheet.shape)
        if args.full:
            # the whole frame at full size: first and last mode side by side (off | 4x)
            pair = np.hstack([label(frames[0], f"{s['name']} {modes[0]}"), np.full((frames[0].shape[0], 6, 3), 30, np.uint8), label(frames[-1], f"{s['name']} {modes[-1]}")])
            cv2.imwrite(os.path.join(args.out, f"{s['name']}_full.png"), pair)
    path = os.path.join(args.out, 'replay_summaries.json')
    old = json.load(open(path)) if os.path.exists(path) else {}
    old.update({f'{k[0]}/{k[1]}/s{k[2]}': v for k, v in summaries.items()})
    json.dump(old, open(path, 'w'), indent=1)


if __name__ == '__main__':
    main()
