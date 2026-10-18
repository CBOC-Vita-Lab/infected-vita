#!/usr/bin/env python3
"""Procedurally generates the LiveArea assets of the selftest package: plain coloured, 8-bit indexed PNGs with the title text, plus
template.xml. No artwork, no game data. usage: gen_livearea.py OUT_DIR [TITLE] [SUBTITLE]
Writes OUT_DIR/sce_sys/{icon0.png (128x128), pic0.png (960x544)} and OUT_DIR/sce_sys/livearea/contents/
{bg.png (840x500), logo0.png (438x181), startup.png (280x158), template.xml}. Same sizes as vitasdk's pretty_livearea sample."""
import os, sys
from PIL import Image, ImageDraw, ImageFont

out = sys.argv[1] if len(sys.argv) > 1 else "livearea"
title = sys.argv[2] if len(sys.argv) > 2 else "Infected Port Self-Test"
sub = sys.argv[3] if len(sys.argv) > 3 else "INFT00001"

FONTS = ["/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf"]
def font(size):
    for f in FONTS:
        if os.path.exists(f):
            return ImageFont.truetype(f, size)
    return ImageFont.load_default(size)   # Pillow >= 10.1 scalable built-in font

def fit_font(draw, text, max_w, start):
    size = start
    while size > 8:
        f = font(size)
        if draw.textlength(text, font=f) <= max_w:
            return f
        size -= 2
    return font(8)

def panel(w, h, bg, fg, lines, margin=0.08):
    """Flat colour with a darker border stripe and centred text lines [(text, relative_size)]. Returns an 8-bit palette image."""
    im = Image.new("RGB", (w, h), bg)
    d = ImageDraw.Draw(im)
    d.rectangle([0, 0, w - 1, h - 1], outline=fg, width=max(2, w // 120))
    max_w = int(w * (1 - 2 * margin))
    total = 0
    fonts = []
    for text, rel in lines:
        f = fit_font(d, text, max_w, int(h * rel))
        fonts.append(f)
        total += f.size * 1.25
    y = (h - total) / 2
    for (text, rel), f in zip(lines, fonts):
        tw = d.textlength(text, font=f)
        d.text(((w - tw) / 2, y), text, font=f, fill=fg)
        y += f.size * 1.25
    return im.quantize(colors=16, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)

def save(im, rel):
    path = os.path.join(out, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    # The Vita installer rejects indexed PNGs below 8 bits per pixel (promote fails with 0x8010113D on hardware).
    im.save(path, bits=8)
    print("  %-48s %dx%d %s %d bytes" % (rel, im.size[0], im.size[1], im.mode, os.path.getsize(path)))

BG = (24, 40, 72)
FG = (255, 200, 40)
save(panel(128, 128, BG, FG, [("INFECTED", 0.17), ("SELF", 0.17), ("TEST", 0.17)]), "sce_sys/icon0.png")
save(panel(960, 544, (16, 24, 48), (90, 140, 230), [(title, 0.10), (sub, 0.06)]), "sce_sys/pic0.png")
save(panel(840, 500, (20, 32, 60), (70, 110, 190), [(title, 0.10)]), "sce_sys/livearea/contents/bg.png")
save(panel(438, 181, BG, FG, [(title, 0.20), (sub, 0.12)]), "sce_sys/livearea/contents/logo0.png")
save(panel(280, 158, (60, 24, 24), (255, 230, 120), [("START TEST", 0.22)]), "sce_sys/livearea/contents/startup.png")

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
p = os.path.join(out, "sce_sys/livearea/contents/template.xml")
os.makedirs(os.path.dirname(p), exist_ok=True)
open(p, "w").write(xml)
print("  sce_sys/livearea/contents/template.xml")
