#!/usr/bin/env python3
"""Builds the bubble (LiveArea) pictures and a RetroFlow cover from the player's own disc image and, optionally, a
screenshot of the port running. Nothing of the game is in this repository: the pictures are made on the player's machine
from the ISO they installed (PSP_GAME/ICON0.PNG, the 144x80 icon, and PSP_GAME/PIC1.PNG, the 480x272 background of the
PSP's menu), and stay there.

usage: art_from_disc.py ISO OUT_DIR [SCREENSHOT.png]     (the screenshot, if given, is only RetroFlow's background)

Writes, in the sizes the Vita and vitasdk's pretty_livearea sample use (every LiveArea picture an 8-bit indexed PNG: the
installer refuses other kinds):
  OUT_DIR/livearea/sce_sys/icon0.png                       128x128  the bubble
  OUT_DIR/livearea/sce_sys/pic0.png                        960x544
  OUT_DIR/livearea/sce_sys/livearea/contents/bg.png        840x500
  OUT_DIR/livearea/sce_sys/livearea/contents/startup.png   280x158  the gate
  OUT_DIR/livearea/sce_sys/livearea/contents/logo0.png     438x181
  OUT_DIR/livearea/sce_sys/livearea/contents/template.xml
  OUT_DIR/retroflow/COVERS/Homebrew/<TITLEID>.png          250x320  RetroFlow's cover size
  OUT_DIR/retroflow/BACKGROUNDS/Homebrew/<TITLEID>.png     960x544  (the menu background, or the screenshot)
The livearea/ tree is what build.sh's LIVEAREA_DIR wants; the retroflow/ tree goes to ux0:data/RetroFlow/.
Needs Pillow and pycdlib (pip install pycdlib)."""
import io
import os
import sys

import pycdlib
from PIL import Image, ImageDraw, ImageEnhance, ImageFilter

TITLE_ID = os.environ.get("TITLE_ID", "INFC00001")


def from_iso(iso_path, name):
    iso = pycdlib.PyCdlib()
    iso.open(iso_path)
    try:
        data = io.BytesIO()
        iso.get_file_from_iso_fp(data, iso_path="/PSP_GAME/" + name)
        return Image.open(io.BytesIO(data.getvalue())).convert("RGB")
    finally:
        iso.close()


def cover(im, w, h, anchor_x=0.5, anchor_y=0.5):
    """Scaled to fill w x h, the rest cut off around the anchor (0..1 of the spare width and height)."""
    scale = max(w / im.width, h / im.height)
    big = im.resize((max(w, round(im.width * scale)), max(h, round(im.height * scale))), Image.LANCZOS)
    x = round((big.width - w) * anchor_x)
    y = round((big.height - h) * anchor_y)
    return big.crop((x, y, x + w, y + h))


def fit(im, w, h):
    scale = min(w / im.width, h / im.height)
    return im.resize((max(1, round(im.width * scale)), max(1, round(im.height * scale))), Image.LANCZOS)


def paste_center(base, im, cx, cy):
    base.paste(im, (round(cx - im.width / 2), round(cy - im.height / 2)))


def shadowed(base, im, x, y, blur=4, strength=200):
    """Pastes im at (x, y) over a soft dark shadow, so that it stands out from a busy background."""
    mask = Image.new("L", base.size, 0)
    ImageDraw.Draw(mask).rectangle([x - 2, y - 2, x + im.width + 2, y + im.height + 2], fill=strength)
    mask = mask.filter(ImageFilter.GaussianBlur(blur))
    base.paste(Image.new("RGB", base.size, (0, 0, 0)), (0, 0), mask)
    base.paste(im, (x, y))


def duotone(im, dark=(0, 0, 0), middle=(130, 14, 6), light=(255, 186, 110)):
    """The picture's brightness mapped from black through blood red to the icon's orange."""
    lut = []
    for c in range(3):
        for v in range(256):
            t = v / 255.0
            a, b, u = (dark[c], middle[c], t * 2) if t < 0.5 else (middle[c], light[c], t * 2 - 1)
            lut.append(round(a + (b - a) * u))
    return im.convert("L").convert("RGB").point(lut)


def indexed(im):
    return im.convert("RGB").quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.FLOYDSTEINBERG)


def save(im, root, rel, palette=True):
    path = os.path.join(root, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    (indexed(im) if palette else im.convert("RGB")).save(path, bits=8, optimize=True)
    print("  %-56s %dx%d %d bytes" % (rel, im.width, im.height, os.path.getsize(path)))


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    iso_path, out = sys.argv[1], sys.argv[2]
    shot = Image.open(sys.argv[3]).convert("RGB") if len(sys.argv) > 3 else None
    icon = from_iso(iso_path, "ICON0.PNG")   # 144x80: the red hand and the title
    pic1 = from_iso(iso_path, "PIC1.PNG")    # 480x272: the title over the infected, grey
    la = os.path.join(out, "livearea")

    # The bubble: the icon's glow enlarged to fill it, the hand large in the middle and the title across, inside the
    # round mask the home screen cuts.
    bubble = cover(icon, 128, 128).filter(ImageFilter.GaussianBlur(6))
    hand = icon.crop((44, 0, 100, 47))
    hand = hand.resize((round(hand.width * 1.75), round(hand.height * 1.75)), Image.LANCZOS)
    glow = Image.new("L", hand.size, 0)
    ImageDraw.Draw(glow).ellipse([4, 4, hand.width - 5, hand.height - 5], fill=255)
    bubble.paste(hand, ((128 - hand.width) // 2, 6), glow.filter(ImageFilter.GaussianBlur(8)))
    title = icon.crop((6, 45, 138, 68))
    title = fit(title, 112, 40)
    shadowed(bubble, title, (128 - title.width) // 2, 86, blur=3, strength=150)
    save(bubble, la, "sce_sys/icon0.png")

    # The picture behind the bubble's start screen and the LiveArea background: the PSP menu's background.
    save(pic1.resize((960, 544), Image.LANCZOS), la, "sce_sys/pic0.png")
    bg = ImageEnhance.Brightness(cover(pic1, 840, 500)).enhance(0.8)
    save(bg, la, "sce_sys/livearea/contents/bg.png")

    # The gate: the icon on black.
    gate = Image.new("RGB", (280, 158), (0, 0, 0))
    paste_center(gate, fit(icon, 276, 154), 140, 79)
    save(gate, la, "sce_sys/livearea/contents/startup.png")

    # The frame below the gate: the title card itself (the icon at full height over its own enlarged, blurred glow).
    card = icon.resize((438, 181), Image.LANCZOS).filter(ImageFilter.GaussianBlur(28))   # smeared into a plain glow of the icon's colours
    card = ImageEnhance.Brightness(card).enhance(0.85)
    paste_center(card, fit(icon, 438, 181), 219, 90)
    save(card, la, "sce_sys/livearea/contents/logo0.png")
    path = os.path.join(la, "sce_sys/livearea/contents/template.xml")
    with open(path, "w") as f:
        f.write('<?xml version="1.0" encoding="utf-8"?>\n<livearea style="psmobile" format-ver="01.00" content-rev="1">\n'
                '\t<livearea-background>\n\t\t<image>bg.png</image>\n\t</livearea-background>\n'
                '\t<gate>\n\t\t<startup-image>startup.png</startup-image>\n\t</gate>\n'
                '\t<frame id="frame1">\n\t\t<liveitem>\n\t\t\t<image>logo0.png</image>\n\t\t</liveitem>\n\t</frame>\n</livearea>\n')

    # RetroFlow's cover (250x320, the size of its own covers): the infected of the menu's background, the hand and the
    # title above them, a red rule at the foot.
    rf = os.path.join(out, "retroflow")
    card = cover(pic1, 250, 320, 0.85, 0.5)
    card = duotone(ImageEnhance.Brightness(ImageEnhance.Contrast(card).enhance(1.5)).enhance(1.25))
    shade = Image.linear_gradient("L").resize((250, 320)).transpose(Image.FLIP_TOP_BOTTOM)
    shade = shade.point(lambda v: max(0, v - 90) * 255 // 165)   # dark at the top, clear from a third down
    card.paste(Image.new("RGB", card.size, (0, 0, 0)), (0, 0), shade)
    logo = fit(icon, 250, 139)
    feather = Image.new("L", logo.size, 0)
    ImageDraw.Draw(feather).rectangle([10, 0, logo.width - 11, logo.height - 14], fill=255)
    card.paste(logo, (0, 0), feather.filter(ImageFilter.GaussianBlur(9)))
    ImageDraw.Draw(card).rectangle([0, 312, 249, 319], fill=(150, 12, 12))
    save(card, rf, "COVERS/Homebrew/%s.png" % TITLE_ID, palette=False)
    # RetroFlow's background: the same picture the bubble's start screen uses (a screenshot of the port, when one is given).
    save(cover(shot if shot is not None else pic1, 960, 544), rf, "BACKGROUNDS/Homebrew/%s.png" % TITLE_ID, palette=False)


if __name__ == "__main__":
    main()
