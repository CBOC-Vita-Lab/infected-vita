#!/usr/bin/env python3
"""sheet.py OUT.png TITLE ROWLABEL=img1,img2,img3 ... --cols 100%,150%,200% [--diff] : comparison sheet (960x544 shots scaled to half size),
one row per scene, one column per setting; --diff adds a column with the amplified absolute difference between the first and last image,
and prints the mean absolute difference over the whole picture and over the HUD regions (radar, top bar, weapon boxes)."""
import sys
from PIL import Image, ImageDraw, ImageChops, ImageFont
import numpy as np

args = sys.argv[1:]
out, title = args[0], args[1]
cols = []
diff = False
rows = []
i = 2
while i < len(args):
    if args[i] == '--cols':
        cols = args[i + 1].split(','); i += 2
    elif args[i] == '--diff':
        diff = True; i += 1
    else:
        label, files = args[i].split('=', 1)
        rows.append((label, files.split(','))); i += 1
W, H = 480, 272
try:
    font = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf', 15)
except OSError:
    font = ImageFont.load_default()
ncol = len(cols) + (1 if diff else 0)
head, pad = 34, 22
sheet = Image.new('RGB', (ncol * W, head + len(rows) * (H + pad)), (20, 20, 24))
d = ImageDraw.Draw(sheet)
d.text((8, 8), title, fill=(255, 255, 255), font=font)
HUD = {'radar': (735, 5, 960, 230), 'top bar': (280, 5, 680, 40), 'weapon boxes': (0, 460, 960, 544), 'score/time': (0, 5, 240, 80)}
for r, (label, files) in enumerate(rows):
    y = head + r * (H + pad)
    imgs = [Image.open(f).convert('RGB') for f in files]
    for c, im in enumerate(imgs):
        sheet.paste(im.resize((W, H), Image.LANCZOS), (c * W, y + pad))
        d.text((c * W + 6, y + 3), '%s   %s' % (cols[c], label), fill=(255, 220, 120), font=font)
    if diff:
        a = np.asarray(imgs[0]).astype(int); b = np.asarray(imgs[-1]).astype(int)
        dd = np.abs(a - b).sum(axis=2)
        vis = Image.fromarray(np.clip(dd * 4, 0, 255).astype('uint8')).resize((W, H))
        sheet.paste(vis.convert('RGB'), (len(imgs) * W, y + pad))
        d.text((len(imgs) * W + 6, y + 3), '|%s - %s| x4' % (cols[0], cols[-1]), fill=(255, 220, 120), font=font)
        parts = ['whole %.2f' % (dd.mean() / 3.0)]
        for name, (x0, y0, x1, y1) in HUD.items():
            parts.append('%s %.2f' % (name, dd[y0:y1, x0:x1].mean() / 3.0))
        print(label, 'mean abs diff per channel (0..255):', ', '.join(parts))
sheet.save(out)
print('wrote', out, sheet.size)
