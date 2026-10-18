#!/usr/bin/env python3
"""Builds the bubble (LiveArea) pictures and RetroFlow's cover and background from pictures you provide (fan art, a poster, a
screenshot). Nothing is drawn or added: each picture is the one you give, cropped and scaled to the sizes the Vita uses.
Same output layout as art_from_disc.py and gen_bubble.py, so build.sh --art OUT_DIR takes it.

usage: art_from_image.py IMAGE OUT_DIR [--cover IMAGE] [--background IMAGE] [--focus X,Y] [--cover-focus X,Y]
  IMAGE        the main picture: the bubble, the start screen, the LiveArea background and, unless given, RetroFlow's background
  --cover      a portrait picture for RetroFlow's cover (default: IMAGE)
  --background a picture for RetroFlow's background (default: IMAGE)
  --focus      where the subject is, as fractions of the width and height (default 0.5,0.5): the crops keep it in view
  --cover-focus  the same for the cover (default: --focus)
Pictures that are yours or that you may use stay on your machine: do not commit the output or the input.
Needs Pillow."""
import os
import sys

from PIL import Image, ImageEnhance, ImageOps

TITLE_ID = os.environ.get("TITLE_ID", "INFC00001")


def load(path):
    im = ImageOps.exif_transpose(Image.open(path))
    if im.mode in ("RGBA", "LA", "P"):
        bg = Image.new("RGB", im.size, (0, 0, 0))
        im = im.convert("RGBA")
        bg.paste(im, mask=im.split()[3])
        return bg
    return im.convert("RGB")


def crop(im, w, h, focus):
    """Scaled to fill w x h, the rest cut off so that the focus point stays as near the middle as the edges allow."""
    scale = max(w / im.width, h / im.height)
    big = im.resize((max(w, round(im.width * scale)), max(h, round(im.height * scale))), Image.LANCZOS)
    x = min(max(round(big.width * focus[0] - w / 2), 0), big.width - w)
    y = min(max(round(big.height * focus[1] - h / 2), 0), big.height - h)
    return big.crop((x, y, x + w, y + h))


def indexed(im):
    return im.convert("RGB").quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.FLOYDSTEINBERG)


def save(im, root, rel, palette=True):
    path = os.path.join(root, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    (indexed(im).save(path, bits=8, optimize=True) if palette else im.convert("RGB").save(path, optimize=True))
    print("  %-56s %dx%d %d bytes" % (rel, im.width, im.height, os.path.getsize(path)))


def point(text):
    x, y = (float(v) for v in text.split(","))
    return (min(max(x, 0.0), 1.0), min(max(y, 0.0), 1.0))


def main():
    args = sys.argv[1:]
    opts = {"--cover": None, "--background": None, "--focus": "0.5,0.5", "--cover-focus": None}
    pos = []
    i = 0
    while i < len(args):
        if args[i] in opts and i + 1 < len(args):
            opts[args[i]] = args[i + 1]
            i += 2
        else:
            pos.append(args[i])
            i += 1
    if len(pos) != 2:
        sys.exit(__doc__)
    image, out = pos
    main_im = load(image)
    cover_im = load(opts["--cover"]) if opts["--cover"] else main_im
    back_im = load(opts["--background"]) if opts["--background"] else main_im
    focus = point(opts["--focus"])
    cover_focus = point(opts["--cover-focus"]) if opts["--cover-focus"] else focus
    la = os.path.join(out, "livearea")
    rf = os.path.join(out, "retroflow")

    save(crop(main_im, 128, 128, focus), la, "sce_sys/icon0.png")
    save(crop(main_im, 960, 544, focus), la, "sce_sys/pic0.png")
    save(ImageEnhance.Brightness(crop(main_im, 840, 500, focus)).enhance(0.85), la, "sce_sys/livearea/contents/bg.png")
    save(crop(main_im, 438, 181, focus), la, "sce_sys/livearea/contents/logo0.png")
    save(crop(main_im, 280, 158, focus), la, "sce_sys/livearea/contents/startup.png")
    path = os.path.join(la, "sce_sys/livearea/contents/template.xml")
    with open(path, "w") as f:
        f.write('<?xml version="1.0" encoding="utf-8"?>\n<livearea style="psmobile" format-ver="01.00" content-rev="1">\n'
                '\t<livearea-background>\n\t\t<image>bg.png</image>\n\t</livearea-background>\n'
                '\t<gate>\n\t\t<startup-image>startup.png</startup-image>\n\t</gate>\n'
                '\t<frame id="frame1">\n\t\t<liveitem>\n\t\t\t<image>logo0.png</image>\n\t\t</liveitem>\n\t</frame>\n</livearea>\n')
    save(crop(cover_im, 250, 320, cover_focus), rf, "COVERS/Homebrew/%s.png" % TITLE_ID, palette=False)
    save(crop(back_im, 960, 544, focus), rf, "BACKGROUNDS/Homebrew/%s.png" % TITLE_ID, palette=False)


if __name__ == "__main__":
    main()
