"""BC1 (DXT1) / BC3 (DXT5) encoding through libbcenc.so (rgbcx) and decoding for checks (texture2ddecoder)."""
import ctypes
import os

import numpy as np

_DEFAULT_LIB = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'bcenc', 'build', 'libbcenc.so')


class Encoder:
    def __init__(self, lib_path=None):
        lib_path = lib_path or os.environ.get('BCENC_LIB') or _DEFAULT_LIB
        self.lib = ctypes.CDLL(lib_path)
        self.lib.bcenc_init()
        self.lib.bcenc_encode.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_void_p]

    def encode(self, rgba, bc3, level=10):
        """rgba uint8 (H, W, 4) -> block bytes. Sizes that are not multiples of 4 are padded by edge replication."""
        h, w = rgba.shape[:2]
        ph, pw = (h + 3) // 4 * 4, (w + 3) // 4 * 4
        if (ph, pw) != (h, w):
            rgba = np.pad(rgba, ((0, ph - h), (0, pw - w), (0, 0)), mode='edge')
        rgba = np.ascontiguousarray(rgba)
        out = (ctypes.c_uint8 * ((pw // 4) * (ph // 4) * (16 if bc3 else 8)))()
        self.lib.bcenc_encode(rgba.ctypes.data, pw, ph, 1 if bc3 else 0, level, out)
        return bytes(out)


def decode(data, w, h, bc3):
    """Blocks -> RGBA uint8 (h, w, 4) (the cropped size)."""
    import texture2ddecoder
    pw, ph = (w + 3) // 4 * 4, (h + 3) // 4 * 4
    raw = texture2ddecoder.decode_bc3(data, pw, ph) if bc3 else texture2ddecoder.decode_bc1(data, pw, ph)
    img = np.frombuffer(raw, np.uint8).reshape(ph, pw, 4)[:h, :w]
    return img[..., [2, 1, 0, 3]].copy()   # BGRA -> RGBA


def psnr(a, b, channels=3):
    d = a[..., :channels].astype(np.float64) - b[..., :channels].astype(np.float64)
    mse = (d * d).mean()
    return 99.0 if mse == 0 else 10 * np.log10(255.0 * 255.0 / mse)
