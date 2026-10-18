#!/usr/bin/env python3
"""The home-screen bubble (LiveArea) of the Infected port: original artwork drawn by this script. No game art, no fonts
or pictures from anywhere else (the default scalable font of the machine that runs it), so the pictures can sit in the
public repository and in every build.

usage: gen_bubble.py OUT_DIR [TITLE] [SUBTITLE] [TITLEID]
Writes the same two trees as art_from_disc.py (the build takes either one: build.sh --art DIR).
The bubble, in the sizes the Vita and vitasdk's pretty_livearea sample use (every picture an 8-bit indexed PNG: the installer
refuses other kinds, and indexed pictures below 8 bits too; promote fails with 0x8010113D):
  OUT_DIR/livearea/sce_sys/icon0.png                       128x128  the bubble
  OUT_DIR/livearea/sce_sys/pic0.png                        960x544
  OUT_DIR/livearea/sce_sys/livearea/contents/bg.png        840x500  the LiveArea background
  OUT_DIR/livearea/sce_sys/livearea/contents/logo0.png     438x181  the title card
  OUT_DIR/livearea/sce_sys/livearea/contents/startup.png   280x158  the gate (the button that starts the game)
  OUT_DIR/livearea/sce_sys/livearea/contents/template.xml
and the RetroFlow launcher's art (plain RGB PNGs, the kind RetroFlow's own covers are; the app copies them into
ux0:data/RetroFlow/ on its first start when RetroFlow is installed):
  OUT_DIR/retroflow/COVERS/Homebrew/<TITLEID>.png          250x320
  OUT_DIR/retroflow/BACKGROUNDS/Homebrew/<TITLEID>.png     960x544
Needs Pillow. For the game's own art instead, see art_from_disc.py (made on the player's machine from their own disc)."""
import math
import os
import random
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

out = sys.argv[1] if len(sys.argv) > 1 else "livearea"
title = sys.argv[2] if len(sys.argv) > 2 else "INFECTED"
sub = sys.argv[3] if len(sys.argv) > 3 else "NATIVE PS VITA PORT"
title_id = sys.argv[4] if len(sys.argv) > 4 else "INFC00001"

GREEN = (150, 255, 60)        # the glow of the infection
GREEN_DIM = (60, 150, 40)
DARK_TOP = (3, 12, 8)
DARK_BOTTOM = (10, 38, 22)
WHITE = (236, 255, 226)

FONTS = ["/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
         "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf", "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf"]


def font(size):
    for f in FONTS:
        if os.path.exists(f):
            return ImageFont.truetype(f, int(size))
    return ImageFont.load_default(int(size))   # Pillow >= 10.1: a scalable built-in font


def background(w, h, seed):
    """Dark green gradient, a vignette, a faint hexagon grid and a few drifting spores."""
    rnd = random.Random(seed)
    im = Image.new("RGB", (w, h))
    px = ImageDraw.Draw(im)
    for y in range(h):
        t = y / max(1, h - 1)
        px.line([(0, y), (w, y)], fill=tuple(int(DARK_TOP[i] + (DARK_BOTTOM[i] - DARK_TOP[i]) * t) for i in range(3)))
    grid = Image.new("L", (w, h), 0)
    gd = ImageDraw.Draw(grid)
    r = max(18, h // 11)
    dx, dy = r * 1.5, r * math.sqrt(3)
    for col in range(-1, int(w / dx) + 2):
        for row in range(-1, int(h / dy) + 2):
            cx = col * dx
            cy = row * dy + (dy / 2 if col % 2 else 0)
            pts = [(cx + r * math.cos(math.radians(60 * k)), cy + r * math.sin(math.radians(60 * k))) for k in range(6)]
            gd.line(pts + [pts[0]], fill=255, width=1)
    grid = grid.point(lambda v: v * 22 // 255)
    im = Image.composite(Image.new("RGB", (w, h), GREEN_DIM), im, grid)
    # Spores: small soft dots in the brighter lower part.
    spores = Image.new("RGB", (w, h), (0, 0, 0))
    sd = ImageDraw.Draw(spores)
    for _ in range(max(12, w * h // 9000)):
        x, y = rnd.uniform(0, w), rnd.uniform(h * 0.15, h)
        rad = rnd.choice([1.5, 2, 2, 3, 4])
        a = rnd.uniform(0.25, 0.9)
        c = tuple(int(v * a) for v in GREEN)
        sd.ellipse([x - rad, y - rad, x + rad, y + rad], fill=c)
    spores = spores.filter(ImageFilter.GaussianBlur(1.2))
    im = ImageChops.add(im, spores)
    # Vignette.
    vig = Image.new("L", (w, h), 0)
    vd = ImageDraw.Draw(vig)
    vd.ellipse([-w * 0.25, -h * 0.35, w * 1.25, h * 1.35], fill=255)
    vig = vig.filter(ImageFilter.GaussianBlur(min(w, h) * 0.12))
    return Image.composite(im, Image.new("RGB", (w, h), (1, 5, 3)), vig)


def emblem(size):
    """A virus particle: a glowing core, a ring, and spikes with round tips. RGBA on transparent, size x size."""
    s = 4                                     # drawn at 4x, shrunk at the end
    n = size * s
    layer = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    c = n / 2
    spikes = 12
    ring_r = n * 0.255
    for k in range(spikes):
        a = math.radians(360 * k / spikes + 15)
        x0, y0 = c + math.cos(a) * ring_r, c + math.sin(a) * ring_r
        x1, y1 = c + math.cos(a) * n * 0.40, c + math.sin(a) * n * 0.40
        d.line([(x0, y0), (x1, y1)], fill=GREEN + (255,), width=max(2, int(n * 0.035)))
        tip = n * (0.052 if k % 2 == 0 else 0.038)
        d.ellipse([x1 - tip, y1 - tip, x1 + tip, y1 + tip], fill=GREEN + (255,))
    d.ellipse([c - ring_r, c - ring_r, c + ring_r, c + ring_r], fill=(8, 40, 14, 255), outline=GREEN + (255,), width=max(3, int(n * 0.03)))
    # Core: three lobes cut into a bright disc, the way a hazard sign has them.
    core_r = ring_r * 0.62
    d.ellipse([c - core_r, c - core_r, c + core_r, c + core_r], fill=(190, 255, 110, 255))
    for k in range(3):
        a = math.radians(120 * k - 90)
        ox, oy = c + math.cos(a) * core_r * 0.52, c + math.sin(a) * core_r * 0.52
        rr = core_r * 0.36
        d.ellipse([ox - rr, oy - rr, ox + rr, oy + rr], fill=(8, 40, 14, 255))
    d.ellipse([c - core_r * 0.16, c - core_r * 0.16, c + core_r * 0.16, c + core_r * 0.16], fill=(8, 40, 14, 255))
    # Glow: the same shape blurred behind it.
    glow = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    glow.paste(Image.new("RGBA", (n, n), GREEN + (255,)), mask=layer.split()[3])
    glow = glow.filter(ImageFilter.GaussianBlur(n * 0.035))
    glow.putalpha(glow.split()[3].point(lambda v: v * 6 // 10))
    both = Image.alpha_composite(glow, layer)
    return both.resize((size, size), Image.LANCZOS)


def fit(text, max_w, start, track=0):
    """The largest font size <= start whose text, with track pixels between letters, is at most max_w wide."""
    size = start
    probe = ImageDraw.Draw(Image.new("L", (8, 8)))
    while size > 8:
        f = font(size)
        if sum(probe.textlength(ch, font=f) + track for ch in text) - track <= max_w:
            return f
        size -= 1
    return font(8)


def text_glow(im, xy, text, f, fill=WHITE, glow=GREEN, anchor="la", track=0):
    """Text with a glow behind it. track: extra pixels between letters."""
    layer = Image.new("RGBA", im.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    x, y = xy
    if track:
        widths = [d.textlength(ch, font=f) + track for ch in text]
        total = sum(widths) - track
        if anchor[0] == "m":
            x -= total / 2
        elif anchor[0] == "r":
            x -= total
        for ch, wd in zip(text, widths):
            d.text((x, y), ch, font=f, fill=fill + (255,), anchor="l" + anchor[1])
            x += wd
    else:
        d.text((x, y), text, font=f, fill=fill + (255,), anchor=anchor)
    g = Image.new("RGBA", im.size, glow + (255,))
    g.putalpha(layer.split()[3].filter(ImageFilter.GaussianBlur(max(2, f.size * 0.09))).point(lambda v: v * 7 // 10))
    base = Image.alpha_composite(im.convert("RGBA"), g)
    return Image.alpha_composite(base, layer).convert("RGB")


def paste_emblem(im, size, cx, cy, alpha=1.0):
    e = emblem(size)
    if alpha < 1.0:
        e.putalpha(e.split()[3].point(lambda v: int(v * alpha)))
    base = im.convert("RGBA")
    base.alpha_composite(e, (int(cx - size / 2), int(cy - size / 2)))
    return base.convert("RGB")


def border(im, width=2, colour=GREEN_DIM):
    d = ImageDraw.Draw(im)
    w, h = im.size
    d.rectangle([0, 0, w - 1, h - 1], outline=colour, width=width)
    return im


def indexed(im):
    return im.convert("RGB").quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.FLOYDSTEINBERG)


def save(im, rel, palette=True):
    path = os.path.join(out, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if palette:
        q = indexed(im)
        q.save(path, bits=8, optimize=True)
    else:                                   # RetroFlow loads its covers as plain RGB
        q = im.convert("RGB")
        q.save(path, optimize=True)
    print("  %-56s %dx%d %s %d bytes" % (rel, q.size[0], q.size[1], q.mode, os.path.getsize(path)))


# ---- icon0: the emblem on the gradient
icon = background(128, 128, 1)
icon = paste_emblem(icon, 118, 64, 64)
save(icon, "livearea/sce_sys/icon0.png")

# ---- bg: the LiveArea background, the emblem large and dim on the right
bg = background(840, 500, 2)
bg = paste_emblem(bg, 470, 640, 270, 0.30)
save(bg, "livearea/sce_sys/livearea/contents/bg.png")

# ---- pic0
pic = background(960, 544, 3)
pic = paste_emblem(pic, 400, 770, 272, 0.9)
pic = text_glow(pic, (64, 246), title, fit(title, 560, 112, 12), anchor="lm", track=12)
pic = text_glow(pic, (70, 326), sub, fit(sub, 540, 34, 5), fill=(190, 230, 170), anchor="lm", track=5)
save(pic, "livearea/sce_sys/pic0.png")

# ---- logo0: a title card, opaque (the installer is picky about transparency; a plate with a border is robust)
logo = background(438, 181, 4)
logo = paste_emblem(logo, 150, 86, 90)
logo = text_glow(logo, (170, 76), title, fit(title, 246, 46, 3), anchor="lm", track=3)
logo = text_glow(logo, (172, 118), sub, fit(sub, 244, 16, 2), fill=(190, 230, 170), anchor="lm", track=2)
save(border(logo, 2), "livearea/sce_sys/livearea/contents/logo0.png")

# ---- startup: the gate
gate = background(280, 158, 5)
gate = paste_emblem(gate, 96, 56, 79)
gate = text_glow(gate, (110, 68), title, fit(title, 158, 30, 2), anchor="lm", track=2)
gate = text_glow(gate, (112, 102), "PLAY", font(20), fill=GREEN, glow=GREEN_DIM, anchor="lm", track=5)
save(border(gate, 2), "livearea/sce_sys/livearea/contents/startup.png")

# ---- RetroFlow: the cover (a portrait card, the size of RetroFlow's own covers) and the background (the same picture as pic0,
#      which is what RetroFlow shows for a Vita game)
cover = background(250, 320, 6)
cover = paste_emblem(cover, 214, 125, 112)
cover = text_glow(cover, (125, 236), title, fit(title, 214, 44, 4), anchor="mm", track=4)
cover = text_glow(cover, (125, 270), "NATIVE PS VITA PORT", fit("NATIVE PS VITA PORT", 214, 14, 2), fill=(190, 230, 170), anchor="mm", track=2)
cd = ImageDraw.Draw(cover)
cd.rectangle([0, 304, 249, 319], fill=GREEN_DIM)
cd.rectangle([0, 304, 249, 307], fill=GREEN)
cover = border(cover, 2)
save(cover, "retroflow/COVERS/Homebrew/%s.png" % title_id, palette=False)
save(pic, "retroflow/BACKGROUNDS/Homebrew/%s.png" % title_id, palette=False)

xml = """<?xml version="1.0" encoding="utf-8"?>
<livearea style="psmobile" format-ver="01.00" content-rev="1">
	<livearea-background>
		<image>bg.png</image>
	</livearea-background>
	<gate>
		<startup-image>startup.png</startup-image>
	</gate>
	<frame id="frame1">
		<liveitem>
			<image>logo0.png</image>
		</liveitem>
	</frame>
</livearea>
"""
p = os.path.join(out, "livearea/sce_sys/livearea/contents/template.xml")
os.makedirs(os.path.dirname(p), exist_ok=True)
open(p, "w").write(xml)
print("  livearea/sce_sys/livearea/contents/template.xml")
