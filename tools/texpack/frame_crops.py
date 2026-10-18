#!/usr/bin/env python3
"""Zoomed crops of replayed frames (the output of compare.py: <dir>/frames/<dump>_<mode>_s<scale>_<flip>.png), modes side by side.

    frame_crops.py FRAMES_DIR DUMP FLIP OUT.png X,Y,W,H [X,Y,W,H ...] [--zoom 2] [--modes off,2x,4x] [--scale 2]

One row per box; every cell is the same box of the frame at one mode, enlarged by --zoom with pixel replication (so the GPU's own
filtering is what you see, nothing is smoothed again). Boxes are in pixels of the replayed frame.
"""
import argparse
import os
import sys

import cv2
import numpy as np


def label(img, text):
    img = img.copy()
    cv2.rectangle(img, (0, 0), (img.shape[1], 16), (0, 0, 0), -1)
    cv2.putText(img, text, (3, 12), cv2.FONT_HERSHEY_SIMPLEX, 0.4, (255, 255, 255), 1, cv2.LINE_AA)
    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('frames')
    ap.add_argument('dump')
    ap.add_argument('flip', type=int)
    ap.add_argument('out')
    ap.add_argument('boxes', nargs='+')
    ap.add_argument('--zoom', type=int, default=2)
    ap.add_argument('--modes', default='off,2x,4x')
    ap.add_argument('--scale', type=int, default=2)
    args = ap.parse_args()
    modes = args.modes.split(',')
    frames = []
    for m in modes:
        p = os.path.join(args.frames, f'{args.dump}_{m}_s{args.scale}_{args.flip:06d}.png')
        img = cv2.imread(p, cv2.IMREAD_COLOR)
        if img is None:
            sys.exit(f'missing {p}')
        frames.append(img)
    rows = []
    for box in args.boxes:
        x, y, w, h = (int(v) for v in box.split(','))
        cells = []
        for m, f in zip(modes, frames):
            c = f[y:y + h, x:x + w]
            if args.zoom > 1:
                c = cv2.resize(c, None, fx=args.zoom, fy=args.zoom, interpolation=cv2.INTER_NEAREST)
            cells.append(label(c, f'{args.dump}:{args.flip} {m} {x},{y} {w}x{h} x{args.zoom}'))
        rows.append(np.hstack([np.pad(c, ((0, 0), (0, 4), (0, 0)), constant_values=30) for c in cells]))
    width = max(r.shape[1] for r in rows)
    rows = [np.pad(r, ((0, 4), (0, width - r.shape[1]), (0, 0)), constant_values=30) for r in rows]
    cv2.imwrite(args.out, np.vstack(rows))
    print('wrote', args.out)


if __name__ == '__main__':
    main()
