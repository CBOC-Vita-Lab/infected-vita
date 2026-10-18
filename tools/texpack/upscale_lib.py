"""Image-level upscaling building blocks shared by upscale.py and the evaluation sheets.

All images are numpy uint8 (H, W, 4) RGBA, the renderer's own decode of a texture (extract.py writes them as PNG).
Every engine returns the picture at `scale` times the size; alpha, bleeding and tiling are handled here so the
networks only ever see a clean RGB picture.
"""
import numpy as np
import cv2


# ---- alpha helpers -------------------------------------------------------------------------------------

def alpha_stats(rgba):
    a = rgba[..., 3]
    n = a.size
    zero = float((a == 0).sum()) / n
    opaque = float((a == 255).sum()) / n
    return {'zero': zero, 'opaque': opaque, 'partial': 1.0 - zero - opaque}


def alpha_kind(rgba):
    """'opaque', 'cutout' (almost only 0 and 255) or 'soft' (a real gradient: glows, smoke, fonts)."""
    st = alpha_stats(rgba)
    if st['opaque'] >= 0.999:
        return 'opaque'
    if st['partial'] <= 0.04:
        return 'cutout'
    return 'soft'


def bleed_colour(rgba, threshold=0):
    """Fill the colour of (nearly) transparent texels with the colour of the nearest visible texel, so a filter never
    pulls the hidden colour into the edge (halos)."""
    a = rgba[..., 3]
    hidden = a <= threshold
    if not hidden.any() or hidden.all():
        return rgba[..., :3].copy()
    src = np.where(hidden, 1, 0).astype(np.uint8)
    _, labels = cv2.distanceTransformWithLabels(src, cv2.DIST_L2, 3, labelType=cv2.DIST_LABEL_PIXEL)
    ys, xs = np.nonzero(src == 0)   # visible pixels, in the raster order the labels are numbered in
    # labels are 1-based over the zero pixels in raster order
    rgb = rgba[..., :3]
    out = rgb.copy()
    pick = labels[hidden] - 1
    out[hidden] = rgb[ys[pick], xs[pick]]
    return out


def premultiplied_bleed(rgba):
    """Colour for engines that need no separate alpha: transparent texels take their neighbours' colour."""
    return bleed_colour(rgba, 0)


def upscale_alpha(alpha, scale, kind):
    """alpha: uint8 (H, W). Cut-outs keep a crisp edge (smoothstep around 0.5 of a Lanczos upscale), soft alpha stays soft."""
    h, w = alpha.shape
    if kind == 'opaque':
        return np.full((h * scale, w * scale), 255, np.uint8)
    f = alpha.astype(np.float32) / 255.0
    up = cv2.resize(f, (w * scale, h * scale), interpolation=cv2.INTER_CUBIC)
    if kind == 'cutout':
        # edge transition about one output texel wide: sharp but not aliased
        up = np.clip((up - 0.5) * 2.2 + 0.5, 0.0, 1.0)
        up = up * up * (3.0 - 2.0 * up)
    else:
        up = np.clip(up, 0.0, 1.0)
    return np.rint(up * 255.0).astype(np.uint8)


# ---- plain scalers --------------------------------------------------------------------------------------

def nearest(rgba, scale):
    return np.repeat(np.repeat(rgba, scale, axis=0), scale, axis=1)


def lanczos(rgba, scale):
    h, w = rgba.shape[:2]
    kind = alpha_kind(rgba)
    rgb = bleed_colour(rgba, 0)
    up = cv2.resize(rgb, (w * scale, h * scale), interpolation=cv2.INTER_LANCZOS4)
    return np.dstack([up, upscale_alpha(rgba[..., 3], scale, kind)])


def bicubic(rgba, scale):
    """Plain bicubic (Catmull-Rom like, no ringing worth the name) with bled colour: the conservative scaler for soft sprites and strands."""
    h, w = rgba.shape[:2]
    kind = alpha_kind(rgba)
    rgb = bleed_colour(rgba, 0)
    up = cv2.resize(rgb, (w * scale, h * scale), interpolation=cv2.INTER_CUBIC)
    return np.dstack([up, upscale_alpha(rgba[..., 3], scale, kind)])


def _scale2x_once(img):
    """EPX / Scale2x on an (H, W, C) uint8 picture: edge-aware pixel doubling without new colours."""
    h, w = img.shape[:2]
    p = np.pad(img, ((1, 1), (1, 1), (0, 0)), mode='edge')
    b = p[0:h, 1:w + 1]
    d = p[1:h + 1, 0:w]
    e = p[1:h + 1, 1:w + 1]
    f = p[1:h + 1, 2:w + 2]
    hh = p[2:h + 2, 1:w + 1]

    def eq(x, y):
        return (x == y).all(axis=2, keepdims=True)

    out = np.empty((h * 2, w * 2, img.shape[2]), img.dtype)
    cond = ~eq(b, hh) & ~eq(d, f)
    e0 = np.where(cond & eq(d, b), d, e)
    e1 = np.where(cond & eq(b, f), f, e)
    e2 = np.where(cond & eq(d, hh), d, e)
    e3 = np.where(cond & eq(hh, f), f, e)
    out[0::2, 0::2] = e0
    out[0::2, 1::2] = e1
    out[1::2, 0::2] = e2
    out[1::2, 1::2] = e3
    return out


def scale2x(rgba, scale):
    """Scale2x applied log2(scale) times, on RGBA together (so cut-out edges follow the colour edges)."""
    out = rgba
    s = 1
    while s < scale:
        out = _scale2x_once(out)
        s *= 2
    return out


def sharp_bilinear(rgba, scale):
    """Nearest at an integer factor, then a gentle bilinear back-off: keeps pixel centres crisp, removes stair steps."""
    h, w = rgba.shape[:2]
    kind = alpha_kind(rgba)
    rgb = bleed_colour(rgba, 0)
    big = np.repeat(np.repeat(rgb, scale, 0), scale, 1)
    soft = cv2.GaussianBlur(big, (0, 0), scale * 0.35)
    return np.dstack([soft, upscale_alpha(rgba[..., 3], scale, kind)])


# ---- AI engines -----------------------------------------------------------------------------------------

def tile_pad(rgb, pad, wrap):
    """Pad with wrapped content for repeating textures, with reflected content otherwise."""
    if pad <= 0:
        return rgb
    h, w = rgb.shape[:2]
    mode = 'wrap' if wrap else 'reflect'
    # np.pad needs pad <= size for reflect; chain it for tiny images
    out = rgb
    left = pad
    while left > 0:
        step_y = min(left, out.shape[0] if mode == 'wrap' else out.shape[0] - 1)
        step_x = min(left, out.shape[1] if mode == 'wrap' else out.shape[1] - 1)
        step = max(1, min(step_y, step_x))
        out = np.pad(out, ((step, step), (step, step), (0, 0)), mode=mode)
        left -= step
    return out


class AiEngine:
    """A network on the device named by TEXPACK_DEVICE: cpu (default), mps (Apple silicon) or cuda."""

    def __init__(self, name, models_dir, threads=2, tile=0):
        import os
        import torch
        from models import load_model
        torch.set_num_threads(threads)
        self.torch = torch
        self.name = name
        self.device = os.environ.get('TEXPACK_DEVICE', 'cpu')
        self.net, self.model_scale = load_model(name, models_dir)
        self.net.to(self.device)
        self.tile = tile

    def run_rgb(self, rgb, pad, wrap):
        """rgb uint8 (H, W, 3) -> uint8 (H*s, W*s, 3), s the model's scale."""
        from models import run_model
        torch = self.torch
        h, w = rgb.shape[:2]
        pp = tile_pad(rgb, pad, wrap)
        x = torch.from_numpy(np.ascontiguousarray(pp.transpose(2, 0, 1))).float().div(255.0).to(self.device)
        y = run_model(self.net, x, tile=self.tile)
        s = self.model_scale
        y = y.clamp(0, 1).mul(255).round().byte().cpu().numpy().transpose(1, 2, 0)
        cy, cx = (pp.shape[0] - h) // 2, (pp.shape[1] - w) // 2
        return y[cy * s:(cy + h) * s, cx * s:(cx + w) * s]

    def upscale(self, rgba, scale, wrap=True, pad=16):
        h, w = rgba.shape[:2]
        kind = alpha_kind(rgba)
        rgb = bleed_colour(rgba, 0)
        s = self.model_scale
        up = self.run_rgb(rgb, pad, wrap)
        if s != scale:
            up = cv2.resize(up, (w * scale, h * scale), interpolation=cv2.INTER_AREA if scale < s else cv2.INTER_CUBIC)
        return np.dstack([up, upscale_alpha(rgba[..., 3], scale, kind)])


def stripe_score(rgba):
    """How much a picture is made of alternating lines (interlaced or scan-line art): the change between neighbouring rows (or columns,
    whichever is larger) over the change between rows two apart, premultiplied. About 0.5-1 for pictures, above 3 for stripes.
    A smooth scaler turns such lines into mush or ringing; they are scaled by pixel replication instead."""
    pm = rgba.astype(np.float32)
    pm[..., :3] *= pm[..., 3:4] / 255.0
    best = 0.0
    for a in (pm, pm.transpose(1, 0, 2)):
        if a.shape[0] < 6:
            continue
        d1 = float(np.abs(a[1:] - a[:-1]).mean())
        d2 = float(np.abs(a[2:] - a[:-2]).mean())
        if d1 > 5.0:
            best = max(best, d1 / (d2 + 1e-3))
    return best


# ---- tone matching --------------------------------------------------------------------------------------

def tone_match(src_rgba, up_rgba, scale, sigma=1.5, strength=1.0):
    """Gives a network's picture back the colour and the large-scale brightness of its source.

    The networks repaint: they shift the mean colour of an area by a few levels, dim thin semi-transparent lines, and do not return
    pure black and white exactly. This adds, to the upscaled colour, the bicubic enlargement of what a Gaussian low-pass (sigma in source
    texels) of the source has more than the same low-pass of the picture averaged back down to the source size. Only the low
    frequencies are touched (dither at texel scale and the network's own detail stay), so the picture keeps its detail but the
    average of every area equals the source's. Alpha is not changed.
    """
    h, w = src_rgba.shape[:2]
    src = bleed_colour(src_rgba, 0).astype(np.float32)
    up = up_rgba[..., :3].astype(np.float32)
    down = cv2.resize(up, (w, h), interpolation=cv2.INTER_AREA)
    # the area of the source that is visible counts; hidden texels carry a bled colour, so they agree with the picture
    diff = cv2.GaussianBlur(src, (0, 0), sigma, borderType=cv2.BORDER_REFLECT) - cv2.GaussianBlur(down, (0, 0), sigma, borderType=cv2.BORDER_REFLECT)
    corr = cv2.resize(diff, (w * scale, h * scale), interpolation=cv2.INTER_CUBIC)
    out = np.clip(up + corr * strength, 0, 255)
    return np.dstack([np.rint(out).astype(np.uint8), up_rgba[..., 3]])


def to_rgba_png(path):
    from PIL import Image
    return np.array(Image.open(path).convert('RGBA'))
