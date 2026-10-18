#!/usr/bin/env python3
"""Which picture of a movie is on a screenshot? For measuring the sync of the HD movies: the picture shown at a flip is found
by comparing the screenshot (the game's frame, 960x544) with the pictures of the ORIGINAL movie around where it is expected.

    match_frames.py MOVIE.pmf FIRST LAST SHOT.png [SHOT.png ...]

prints, for every screenshot, the number of the best matching picture of the movie (FIRST..LAST), the mean absolute difference
of the best and of the runner-up (levels of 255, both pictures reduced to 240x136) and the best's margin over the runner-up.
A margin near zero means the scene is so still that the picture cannot be told from its neighbours.
"""
import subprocess
import sys

import numpy as np
from PIL import Image

W, H = 480, 272


def source_frames(movie, first, last):
    """The pictures first..last reduced to 240x136 (8 bit; the whole movie is about 200 MB)."""
    vf = ("select='between(n,%d,%d)',scale=in_range=pc:in_color_matrix=bt601:out_range=pc:flags=accurate_rnd+full_chroma_int,format=rgb24,"
          "scale=%d:%d:flags=area" % (first, last, W // 2, H // 2))
    raw = subprocess.run(["ffmpeg", "-v", "fatal", "-threads", "1", "-i", movie, "-vf", vf, "-fps_mode", "passthrough", "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
                         capture_output=True, check=True).stdout
    size = (W // 2) * (H // 2) * 3
    count = len(raw) // size
    return np.frombuffer(raw[: count * size], dtype=np.uint8).reshape(count, H // 2, W // 2, 3)


def shot_small(path):
    image = Image.open(path).convert("RGB")
    image = image.resize((W // 2, H // 2), Image.BOX)
    return np.asarray(image, dtype=np.float32)


def main(argv):
    if len(argv) < 5:
        sys.stderr.write(__doc__)
        return 2
    movie, first, last = argv[0], int(argv[1]), int(argv[2])
    frames = source_frames(movie, first, last)
    for path in argv[3:]:
        shot = shot_small(path)
        errors = np.concatenate([np.abs(frames[i:i + 128].astype(np.float32) - shot).mean(axis=(1, 2, 3)) for i in range(0, len(frames), 128)])
        order = np.argsort(errors)
        best, second = int(order[0]), int(order[1])
        print("%s best=%d mad=%.2f second=%d mad=%.2f margin=%.2f" % (path, first + best, errors[best], first + second, errors[second], errors[second] - errors[best]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
