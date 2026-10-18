#!/usr/bin/env python3
"""Build texpack_2x.bin / texpack_4x.bin (IFTP version 1, docs/ARCHITECTURE.md section 7) from the upscaled images.

    pack.py --extract private/extract --up private/up --out private/packs [--scales 2,4] [--limit N] [--jobs 2]

For every texture key of the catalog the pack holds the upscaled picture of its image, recoloured to that key's palette,
as DXT1 (opaque), DXT5 (alpha) or RGBA8 (small UI art), with a full mip chain (alpha coverage kept for cut-outs).
Images whose palette the game recolours at run time (characters) or animates (colour-cycled sprites) also get a recolour
record under the image key (see hd_texture_pack.hpp): the picture, its palette, and for every output pixel the palette
index it belongs to, so the renderer can rebuild it for a palette no disc stream contains.

Needs libbcenc.so (bcenc/build.sh) for the DXT encoder (rgbcx).
"""
import argparse
import collections
import hashlib
import json
import multiprocessing
import os
import struct
import subprocess
import sys
import tempfile
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bc  # noqa: E402
import classify  # noqa: E402
import upscale  # noqa: E402
import upscale_lib as U  # noqa: E402

FORMAT_RGBA8, FORMAT_DXT1, FORMAT_DXT5 = 0, 1, 2
FLAG_RECOLOUR = 1
RGBA8_UI_SOURCE_PIXELS = 1024       # UI art up to 32x32 stays uncompressed (hatching, 1-pixel lines)

G = {}   # shared with the workers (fork)


# ---- the raw records ---------------------------------------------------------------------------------------

def parse_raw(path):
    data = open(path, 'rb').read()
    out = []
    at = 0
    while at < len(data):
        assert data[at:at + 4] == b'IFRW'
        fmt, cf, sw, _ = struct.unpack_from('<BBBB', data, at + 8)
        w, h, bw, shift, mask, off = struct.unpack_from('<6H', data, at + 12)
        tl, cl = struct.unpack_from('<II', data, at + 24)
        rec = {'fmt': fmt, 'cf': cf, 'w': w, 'h': h, 'texels': data[at + 32:at + 32 + tl],
               'clut': data[at + 32 + tl:at + 32 + tl + cl], 'head': data[at:at + 32]}
        out.append(rec)
        at += 32 + tl + cl
    return out


def palette_of(rec):
    """(n, 4) uint8 RGBA of the palette a texture of this record reads (entries 0..n-1; shift 0, mask 255)."""
    n = 16 if rec['fmt'] == 4 else 256
    raw = rec['clut']
    if rec['cf'] == 3:
        return np.frombuffer(raw[:n * 4], np.uint8).reshape(-1, 4).copy()
    v = np.frombuffer(raw[:n * 2], '<u2').astype(np.uint32)
    if rec['cf'] == 0:
        r, g, b, a = (v & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, ((v >> 11) & 31) * 255 // 31, np.full_like(v, 255)
    elif rec['cf'] == 1:
        r, g, b, a = (v & 31) * 255 // 31, ((v >> 5) & 31) * 255 // 31, ((v >> 10) & 31) * 255 // 31, ((v >> 15) & 1) * 255
    else:
        r, g, b, a = (v & 15) * 17, ((v >> 4) & 15) * 17, ((v >> 8) & 15) * 17, ((v >> 12) & 15) * 17
    return np.stack([r, g, b, a], axis=1).astype(np.uint8)


def index_images(records, indices, texkey):
    """Palette index of every texel of the given records, from the renderer's own decode: the palette is replaced by a
    ramp whose red channel is the index, so texkey's RGBA output carries the indices (same swizzle, same code)."""
    with tempfile.TemporaryDirectory() as tmp:
        raw = os.path.join(tmp, 'ramps.raw')
        with open(raw, 'wb') as f:
            for i in indices:
                r = records[i]
                n = 16 if r['fmt'] == 4 else 256
                ramp = np.zeros((n, 4), np.uint8)
                ramp[:, 0] = np.arange(n)
                ramp[:, 3] = 255
                head = bytearray(r['head'])
                struct.pack_into('<BBBB', head, 8, r['fmt'], 3, head[10], 0)   # palette format RGBA8888
                struct.pack_into('<II', head, 24, len(r['texels']), n * 4)
                f.write(bytes(head) + r['texels'] + ramp.tobytes())
        # texkey names the files by key; its stdout lists the records in order
        res = subprocess.run([texkey, 'batch', raw, tmp, '--rgba'], check=True, capture_output=True, text=True).stdout.strip().split('\n')
        out = {}
        for i, line in zip(indices, res):
            fields = line.split()
            key, w, h = fields[1], int(fields[2]), int(fields[3])
            px = np.fromfile(os.path.join(tmp, key + '.rgba'), np.uint8).reshape(h, w, 4)
            out[i] = px[..., 0].copy()
        return out


# ---- recolouring --------------------------------------------------------------------------------------------

def assign_indices(up, src_idx, palette, scale, lam_far=4000.0, lam_other=200.0):
    """Palette index of every output pixel: the index of the source texel it lies in, or of a neighbouring texel when that
    one's palette colour is a clearly better match for the (sharpened) upscaled pixel."""
    H, W = src_idx.shape
    pf = palette.astype(np.float32)
    uf = up.astype(np.float32)
    sp = np.pad(src_idx, 1, mode='edge')
    ys = (np.arange(H * scale) % scale + 0.5) / scale
    xs = (np.arange(W * scale) % scale + 0.5) / scale
    best_err = None
    best = None
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            sub = sp[1 + dy:1 + dy + H, 1 + dx:1 + dx + W]
            cand = np.repeat(np.repeat(sub, scale, 0), scale, 1)
            col = pf[cand]
            err = ((uf - col) ** 2).sum(-1)
            if dx or dy:
                dist_x = (xs if dx < 0 else (1 - xs) if dx > 0 else np.zeros_like(xs))[None, :]
                dist_y = (ys if dy < 0 else (1 - ys) if dy > 0 else np.zeros_like(ys))[:, None]
                err = err + lam_other + lam_far * (dist_x ** 2 + dist_y ** 2)
            if best_err is None:
                best_err, best = err, cand.copy()
            else:
                m = err < best_err
                best_err[m] = err[m]
                best[m] = cand[m]
    return best


def transfer(up, idx, base_palette, palette):
    """up + palette[idx] - base_palette[idx], per channel, clamped."""
    delta = palette.astype(np.int16) - base_palette.astype(np.int16)
    out = up.astype(np.int16) + delta[idx]
    return np.clip(out, 0, 255).astype(np.uint8)


def packbits(data):
    """PackBits of a uint8 array (flattened): repeat packets for runs of 3 or more, literal packets otherwise."""
    a = np.ascontiguousarray(data).reshape(-1)
    n = len(a)
    if n == 0:
        return b''
    starts = np.concatenate(([0], np.flatnonzero(a[1:] != a[:-1]) + 1))
    ends = np.concatenate((starts[1:], [n]))
    out = bytearray()
    lit = bytearray()

    def flush():
        i = 0
        while i < len(lit):
            chunk = lit[i:i + 128]
            out.append(len(chunk) - 1)
            out.extend(chunk)
            i += 128
        lit.clear()

    for s, e in zip(starts.tolist(), ends.tolist()):
        length = e - s
        value = int(a[s])
        if length >= 3:
            flush()
            while length >= 3:
                take = min(length, 128)
                if 0 < length - take < 3:
                    take = length - 3     # a repeat packet holds 2..128 texels: never leave 1 or 2 behind it
                out.append(257 - take)
                out.append(value)
                length -= take
        lit.extend(bytes([value]) * length)
    flush()
    return bytes(out)


def unpackbits(data, size):
    out = bytearray()
    i = 0
    while i < len(data):
        n = data[i]
        i += 1
        if n < 128:
            out.extend(data[i:i + n + 1])
            i += n + 1
        elif n > 128:
            out.extend(bytes([data[i]]) * (257 - n))
            i += 1
    assert len(out) == size
    return bytes(out)


# ---- pictures -> blobs --------------------------------------------------------------------------------------

def mip_chain(img, cutout):
    """Levels from img down to 1x1: a box filter on premultiplied colour (a stripe of opaque white between transparent texels averages
    to half-covered white, not to a darker or tinted colour), and for alpha-TESTED cut-outs (`cutout`: world fences, grates, ladders)
    the alpha scaled per level so the share of texels above 0.5 stays what level 0 has (a mip that loses its coverage makes distant
    fences vanish). HUD art is alpha-BLENDED: it must not get that scaling (a half-covered stripe pattern would turn into a
    uniformly half-transparent texture as soon as the HUD is minified, which is what a 4x pack does at 960x544)."""
    levels = [img]
    f = img.astype(np.float32)
    alpha = f[..., 3:4] / 255.0
    pm = np.concatenate([f[..., :3] * alpha, f[..., 3:4]], axis=2)    # premultiplied colour, alpha
    straight = f[..., :3].copy()                                        # for the colour under (nearly) transparent texels
    target = float((f[..., 3] > 127.5).mean()) if cutout else None

    def halve(x):
        h, w = x.shape[:2]
        if h > 1:
            x = (x[0:h // 2 * 2:2] + x[1:h // 2 * 2:2]) * 0.5
        if w > 1:
            x = (x[:, 0:w // 2 * 2:2] + x[:, 1:w // 2 * 2:2]) * 0.5
        return x

    while pm.shape[0] > 1 or pm.shape[1] > 1:
        pm = halve(pm)
        straight = halve(straight)
        a = pm[..., 3:4]
        colour = np.where(a > 0.5, pm[..., :3] / np.maximum(a / 255.0, 1e-3), straight)
        if cutout and target:
            lo, hi = 0.25, 8.0
            for _ in range(14):
                mid = (lo + hi) / 2
                if float((np.minimum(a[..., 0] * mid, 255.0) > 127.5).mean()) < target:
                    lo = mid
                else:
                    hi = mid
            a = np.minimum(a * ((lo + hi) / 2), 255.0)
        out = np.concatenate([np.clip(colour, 0, 255), a], axis=2)
        levels.append(np.clip(np.rint(out), 0, 255).astype(np.uint8))
    return levels


def choose_format(img, cls, src_w, src_h):
    h, w = img.shape[:2]
    if cls == 'ui' and src_w * src_h <= RGBA8_UI_SOURCE_PIXELS:
        return FORMAT_RGBA8
    if min(h, w) < 4:
        return FORMAT_RGBA8
    return FORMAT_DXT1 if (img[..., 3] == 255).all() else FORMAT_DXT5


def encode_level(enc, img, fmt, level):
    if fmt == FORMAT_RGBA8:
        return np.ascontiguousarray(img).tobytes()
    return enc.encode(img, fmt == FORMAT_DXT5, level)


def encode_picture(enc, img, fmt, cutout, level0, level_rest, chain=True):
    levels = mip_chain(img, cutout) if chain else [img]
    blobs = []
    for i, lv in enumerate(levels):
        blobs.append(encode_level(enc, lv, fmt, level0 if i == 0 else level_rest))
    return b''.join(blobs), len(levels)


def build_image(task):
    entry, scale = task
    enc = G['enc']
    up, engine = upscale.load_final(G['extract'], G['up'], entry, scale)
    if up is None:
        return entry['image'], []
    textures = G['by_image'][entry['image']]
    cls = entry['class']
    w, h = entry['width'], entry['height']
    cutout = U.alpha_kind(up) == 'cutout' and cls == 'world'    # alpha-tested in 3D only (see mip_chain)
    fmt = choose_format(up, cls, w, h)
    recolourable = (cls == 'character' or len(textures) >= 2 or entry['image'] in G['recoloured']) and \
        G['records'][textures[0]['raw_index']]['fmt'] in (4, 5)
    records = []
    base = next(t for t in textures if t['key'] == entry['base_key'])
    base_palette = palette_of(G['records'][base['raw_index']]) if recolourable else None
    idx_up = None
    if recolourable:
        idx_up = assign_indices(up, G['indices'][base['raw_index']], base_palette, scale)
    cache = {}
    for t in textures:
        if recolourable and t is not base:
            pal = palette_of(G['records'][t['raw_index']])
            v = transfer(up, idx_up, base_palette, pal)
        else:
            v = up
        fmt_v = fmt if fmt != FORMAT_DXT1 else (FORMAT_DXT1 if (v[..., 3] == 255).all() else FORMAT_DXT5)
        sig = (v.tobytes() if v is not up else b'base', fmt_v)
        if sig in cache:
            blob, mips = cache[sig]
        else:
            blob, mips = encode_picture(enc, v, fmt_v, cutout, G['level0'], G['level_rest'])
            cache[sig] = (blob, mips)
        records.append({'key': int(t['key'], 16), 'flags': 0, 'w': v.shape[1], 'h': v.shape[0], 'fmt': fmt_v, 'mips': mips,
                        'blob': blob})
    if recolourable:
        pal_n = len(base_palette)
        packed = packbits(idx_up)
        level0, _ = encode_picture(enc, up, fmt, cutout, G['level0'], G['level_rest'], chain=False)
        blob = struct.pack('<HHI', pal_n, 1, len(packed)) + base_palette.tobytes() + packed + level0
        records.append({'key': int(base['image_key'], 16), 'flags': FLAG_RECOLOUR, 'w': up.shape[1], 'h': up.shape[0],
                        'fmt': fmt, 'mips': 1, 'blob': blob})
    return entry['image'], records


def write_pack(path, scale, records):
    records = sorted(records, key=lambda r: r['key'])
    for a, b in zip(records, records[1:]):
        assert a['key'] != b['key'], f"duplicate key {a['key']:016x}"
    count = len(records)
    offset = 16 + count * 28
    blob_at = {}
    layout = []
    body = []
    for r in records:
        digest = hashlib.sha1(r['blob'] + bytes([r['fmt'], r['mips']]) + struct.pack('<HH', r['w'], r['h'])).digest()
        if digest in blob_at:
            pos = blob_at[digest]
        else:
            pos = offset
            blob_at[digest] = pos
            body.append(r['blob'])
            offset += len(r['blob'])
        layout.append(pos)
    with open(path + '.tmp', 'wb') as f:
        f.write(b'IFTP' + struct.pack('<III', 1, count, scale))
        for r, pos in zip(records, layout):
            f.write(struct.pack('<QQIHHBBH', r['key'], pos, len(r['blob']), r['w'], r['h'], r['fmt'], r['mips'], r['flags']))
        for b in body:
            f.write(b)
    os.replace(path + '.tmp', path)
    return offset


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--extract', required=True)
    ap.add_argument('--up', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--rt', help='run-time harvest for the draw kinds (same value as upscale.py)')
    ap.add_argument('--scales', default='2,4')
    ap.add_argument('--mixed', default='', help='one more pack with a scale per class, e.g. world=4,fx=4,character=2,ui=2 '
                    '(written to texpack_<--mixed-name>.bin; a class not named gets the largest scale)')
    ap.add_argument('--mixed-name', default='4x_mixed')
    ap.add_argument('--texkey', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'build', 'bin', 'texkey'))
    ap.add_argument('--jobs', type=int, default=2)
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--level0', type=int, default=12, help='rgbcx quality level of mip 0')
    ap.add_argument('--level-rest', type=int, default=8)
    ap.add_argument('--skip-palette-carriers', action='store_true', default=True,
                    help='leave out the 2x2 palette textures (never sampled; the game copies their palettes)')
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    cat = json.load(open(os.path.join(args.extract, 'catalog.json')))
    records = parse_raw(os.path.join(args.extract, 'rasters.raw'))
    plan = upscale.build_plan(args.extract, args.rt)
    by_image = collections.defaultdict(list)
    for t in cat['textures']:
        by_image[t['image']].append(t)
    observed = classify.load_observed(args.rt) if args.rt else {}
    recoloured = classify.recoloured_images(cat, args.rt)
    recoloured |= {e['image'] for e in plan if classify.runtime_recoloured(e['names'])}
    # palette carriers: tiny CLUT8 textures whose palette the game copies into character textures and never samples.
    # A 2x2 texture that was drawn (a flat fade quad) is kept: it costs a few bytes and is then a hit, not a miss.
    drawn = {e['image'] for e in plan if any(k in observed for k in e['keys'])}
    entries = [e for e in plan if not (args.skip_palette_carriers and min(e['width'], e['height']) <= 2 and e['image'] not in drawn)]
    if args.limit:
        entries = entries[:args.limit]
    # index images of every recolourable base
    need = [next(t for t in by_image[e['image']] if t['key'] == e['base_key'])['raw_index'] for e in entries
            if (e['class'] == 'character' or len(by_image[e['image']]) >= 2 or e['image'] in recoloured) and records[by_image[e['image']][0]['raw_index']]['fmt'] in (4, 5)]
    t0 = time.time()
    indices = index_images(records, need, args.texkey) if need else {}
    print(f'index images for {len(need)} recolourable images in {time.time() - t0:.1f}s', flush=True)
    G.update(extract=args.extract, up=args.up, recoloured=recoloured, by_image=by_image, records=records, indices=indices, enc=bc.Encoder(), level0=args.level0,
             level_rest=args.level_rest)
    stats = {}
    jobs_to_run = [(int(s), f'{int(s)}x', None) for s in args.scales.split(',') if s]
    if args.mixed:
        per_class = {kv.split('=')[0]: int(kv.split('=')[1]) for kv in args.mixed.split(',')}
        jobs_to_run.append((max(per_class.values()), args.mixed_name, per_class))
    for scale, name, per_class in jobs_to_run:
        t0 = time.time()
        tasks = [(e, per_class.get(e['class'], scale) if per_class else scale) for e in entries]
        with multiprocessing.Pool(args.jobs) as pool:
            results = pool.map(build_image, tasks, chunksize=4)
        recs = [r for _, rs in results for r in rs]
        missing = sum(1 for _, rs in results if not rs)
        path = os.path.join(args.out, f'texpack_{name}.bin')
        size = write_pack(path, scale, recs)
        fmts = collections.Counter(r['fmt'] for r in recs)
        stats[name] = {'file': path, 'scales_by_class': per_class, 'bytes': size, 'records': len(recs), 'recolour_records': sum(1 for r in recs if r['flags']),
                        'images': len(entries), 'images_missing': missing, 'formats': {'rgba8': fmts[0], 'dxt1': fmts[1], 'dxt5': fmts[2]},
                        'seconds': round(time.time() - t0, 1), 'index_ram_bytes_per_entry': 8, 'index_disk_bytes_per_entry': 28}
        print(json.dumps(stats[name]), flush=True)
    json.dump(stats, open(os.path.join(args.out, "pack_stats.json" if not args.mixed else "pack_stats_" + args.mixed_name + ".json"), "w"), indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main())
