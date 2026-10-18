#!/usr/bin/env python3
# usage: sheet.py OUT.png shot1.png shot2.png ...   (2 columns, half size, labelled)
import sys
from PIL import Image, ImageDraw
out, files = sys.argv[1], sys.argv[2:]
cols = 2
w, h = 480, 272
rows = (len(files) + cols - 1) // cols
sheet = Image.new("RGB", (cols * w, rows * h), (0, 0, 0))
for i, f in enumerate(files):
    im = Image.open(f).convert("RGB")
    ImageDraw.Draw(im).text((4, 4), f.rsplit("_", 1)[-1], fill=(255, 255, 0))
    sheet.paste(im, ((i % cols) * w, (i // cols) * h))
sheet.save(out)
