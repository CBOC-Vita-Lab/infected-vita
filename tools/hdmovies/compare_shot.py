#!/usr/bin/env python3
"""How close is a screenshot to the replacement movie picture it shows? For checking the colour of the HD movies on a device or an emulator
(the picture of a Vita3K screenshot against the same picture decoded straight from the MP4), and for the desktop's own screenshot as the baseline.

    compare_shot.py REPLACEMENT.mp4 SHOT.png [SHOT.png ...] [--first N] [--last N] [--table]

The picture on a screenshot is found by its edges only (so that wrong colours cannot hide it): the screenshot and every picture of the
movie (or of --first..--last) are reduced to 240x136, and the picture whose edge map correlates best with the screenshot's wins. Then, for
that picture, the mean absolute difference per colour channel (R, G, B, levels of 255) and the mean signed difference (screenshot minus
movie: a positive blue number is a bluish screenshot) are printed, also after reducing both to 240x136 (what the viewer sees, not the
scaling filter of the renderer). The movie is converted the way the port's original path converts it: limited-range BT.601 to 8-bit RGB.

A right picture is within 1 to 3 levels per channel and the signed numbers are near zero; a wrong chroma layout is 20 or more.
"""
import argparse
import subprocess
import sys

import numpy as np
from PIL import Image

W, H = 240, 136


def movie_frames(path, first, last):
    select = "" if first is None else "select='between(n,%d,%d)'," % (first, last)
    vf = (select + "scale=in_range=tv:in_color_matrix=bt601:out_range=pc:flags=accurate_rnd+full_chroma_int,format=rgb24,"
          "scale=%d:%d:flags=area" % (W, H))
    raw = subprocess.run(["ffmpeg", "-v", "fatal", "-threads", "1", "-i", path, "-vf", vf, "-fps_mode", "passthrough", "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
                         capture_output=True, check=True).stdout
    size = W * H * 3
    count = len(raw) // size
    return np.frombuffer(raw[: count * size], dtype=np.uint8).reshape(count, H, W, 3).astype(np.float32)


def shot_small(path):
    image = Image.open(path).convert("RGB").resize((W, H), Image.BOX)
    return np.asarray(image, dtype=np.float32)


def luma(a):
    return a[..., 0] * 0.299 + a[..., 1] * 0.587 + a[..., 2] * 0.114


def edges(a):
    """Gradient magnitude of the luma, one map per picture (frames: N x H x W x 3 or H x W x 3), flattened and normalised."""
    y = luma(a)
    gx = np.abs(np.diff(y, axis=-1))[..., :-1, :]
    gy = np.abs(np.diff(y, axis=-2))[..., :, :-1]
    g = (gx + gy).reshape(y.shape[:-2] + (-1,))
    g = g - g.mean(axis=-1, keepdims=True)
    return g / (np.linalg.norm(g, axis=-1, keepdims=True) + 1e-6)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("movie")
    ap.add_argument("shots", nargs="+")
    ap.add_argument("--first", type=int)
    ap.add_argument("--last", type=int)
    ap.add_argument("--table", action="store_true", help="one line per screenshot, tab separated")
    a = ap.parse_args()
    first = a.first
    last = a.last if a.last is not None else (10 ** 9 if first is not None else None)
    frames = movie_frames(a.movie, first, last)
    if len(frames) == 0:
        sys.exit("no pictures decoded")
    offset = first or 0
    ref_edges = edges(frames)
    ref_luma = luma(frames)
    for path in a.shots:
        shot = shot_small(path)
        corr = ref_edges @ edges(shot)
        order = np.argsort(-corr)
        best = int(order[0])
        margin = float(corr[best] - corr[order[1]]) if len(order) > 1 else 0.0
        d = np.abs(ref_luma - luma(shot)).mean(axis=(1, 2))
        diff = shot - frames[best]
        mad = np.abs(diff).mean(axis=(0, 1))
        bias = diff.mean(axis=(0, 1))
        if a.table:
            print("%s\t%d\t%.3f\t%.2f\t%.2f\t%.2f\t%.2f\t%+.2f\t%+.2f\t%+.2f" % (path, best + offset, corr[best], d[best], mad[0], mad[1], mad[2], bias[0], bias[1], bias[2]))
        else:
            print("%s: picture %d (edge correlation %.3f, margin to the runner-up %.3f; mean luma difference %.2f)" % (path, best + offset, corr[best], margin, d[best]))
            print("    mean absolute difference R %.2f  G %.2f  B %.2f   (mean of the three %.2f)" % (mad[0], mad[1], mad[2], mad.mean()))
            print("    mean signed difference   R %+.2f G %+.2f B %+.2f   (screenshot minus movie)" % (bias[0], bias[1], bias[2]))


if __name__ == "__main__":
    main()
