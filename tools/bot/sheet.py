#!/usr/bin/env python3
"""sheet.py OUT.png DIR_OR_FILES... [--cols N] [--scale S] : contact sheet of PNGs (sorted by name) with the file name stamped."""
import sys, glob, os
from PIL import Image, ImageDraw
args = [a for a in sys.argv[1:]]
cols = 4; scale = 0.5
if '--cols' in args:
    i = args.index('--cols'); cols = int(args[i+1]); del args[i:i+2]
if '--scale' in args:
    i = args.index('--scale'); scale = float(args[i+1]); del args[i:i+2]
out = args[0]; files = []
for a in args[1:]:
    files += sorted(glob.glob(os.path.join(a, '*.png')) + glob.glob(os.path.join(a, '*.bmp')), key=lambda p: (len(p), p)) if os.path.isdir(a) else [a]
ims = [Image.open(f).convert('RGB') for f in files]
w, h = ims[0].size; tw, th = int(w*scale), int(h*scale)
rows = (len(ims)+cols-1)//cols
sheet = Image.new('RGB', (cols*tw, rows*th))
d = ImageDraw.Draw(sheet)
for k, (f, im) in enumerate(zip(files, ims)):
    x, y = (k % cols)*tw, (k//cols)*th
    sheet.paste(im.resize((tw, th)), (x, y))
    d.rectangle([x, y, x+tw, y+11], fill=(0, 0, 0)); d.text((x+2, y), os.path.basename(f).replace('.png', ''), fill=(255, 255, 0))
sheet.save(out); print(out, sheet.size, len(ims))
