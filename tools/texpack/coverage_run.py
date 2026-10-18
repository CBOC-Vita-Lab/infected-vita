#!/usr/bin/env python3
"""Coverage of the HD texture pack beyond mission 1: sees every level of the disc through the mission-1 slot of a COPY of the ISO
(iso_swap.py), plays the scripted flow on the desktop build, harvests the textures drawn (INFECTED_TEX_DUMP), records two frames of
gameplay, replays them off / 2x / 4x and checks the pack.

    coverage_run.py --root WS --iso COPY.iso --game build-game/bin/InfectedNative --replay build/bin/ge_replay --packs private/packs \
        --out private/cov --script stab.script  [--levels Mission_5A,MultiLevel_3 | --all] [--flips 7000,8800] [--max-frames 9000]

Per level, in OUT/<level>/: tex/ (the harvest), frames/ (off, 2x, 4x PNGs of the recorded flips), result.json (textures drawn, direct and recolour hits,
misses with names, the replay's pack counters and texture working set). OUT/coverage.tsv has one line per level. Resumable (a level with result.json
is skipped). Only the copy of the ISO is modified. The recorded GE dumps are deleted once replayed (--keep-ge keeps them).
"""
import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import iso_swap  # noqa: E402
from pack_check import read_index  # noqa: E402

LEVELS_DIR = 'PSP_GAME/USRDIR/IMAGE/levels'


def level_names(iso_path):
    iso = iso_swap.Iso(iso_path)
    names = [e['name'][:-len('.stream.zpm')] for e in iso.entries(iso.find(LEVELS_DIR)) if e['name'].endswith('.stream.zpm')]
    return [n for n in names if n.startswith('Mission_') or n.startswith('MultiLevel_')]


def sort_key(n):
    m = re.match(r'([A-Za-z]+)_(\d+)([A-Z]?)', n)
    return (m.group(1), int(m.group(2)), m.group(3))


def run(cmd, env=None, timeout=None, out=None, err=None):
    return subprocess.run(cmd, env=env, stdout=out, stderr=err, timeout=timeout)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', required=True, help='workspace root (holds private/, proj/)')
    ap.add_argument('--iso', required=True, help='a COPY of the disc image (redirected in place)')
    ap.add_argument('--game', required=True)
    ap.add_argument('--replay', required=True)
    ap.add_argument('--packs', required=True, help='directory with texpack_2x.bin and texpack_4x.bin')
    ap.add_argument('--catalog', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--script', required=True, help='pad script (flip keyed) that reaches the mission and plays')
    ap.add_argument('--data-template', required=True, help='directory with EBOOT.ELF and settings.ini for the data dir')
    ap.add_argument('--ms0', required=True, help='pristine memory stick to start from')
    ap.add_argument('--levels', default='')
    ap.add_argument('--all', action='store_true')
    ap.add_argument('--flips', default='7000,8800')
    ap.add_argument('--max-frames', type=int, default=9000)
    ap.add_argument('--seconds', type=int, default=1500)
    ap.add_argument('--keep-ge', action='store_true')
    args = ap.parse_args()

    names = sorted(level_names(args.iso), key=sort_key)
    todo = [n for n in args.levels.split(',') if n] if args.levels else (names if args.all else [])
    unknown = [n for n in todo if n not in names]
    if unknown:
        sys.exit(f'no such level: {unknown}; the disc has {names}')
    cat = json.load(open(args.catalog))['textures']
    by_key = {t['key']: t for t in cat}
    packs = {m: read_index(os.path.join(args.packs, f'texpack_{m}.bin'))[0] for m in ('2x', '4x')}
    flips = [int(f) for f in args.flips.split(',')]
    os.makedirs(args.out, exist_ok=True)
    py = sys.executable
    swap = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'iso_swap.py')
    slot = f'{LEVELS_DIR}/Mission_1A.stream.zpm'
    for name in todo:
        out = os.path.join(args.out, name)
        if os.path.exists(os.path.join(out, 'result.json')):
            continue
        shutil.rmtree(out, ignore_errors=True)
        for sub in ('tex', 'ge', 'frames', 'shots'):
            os.makedirs(os.path.join(out, sub))
        t0 = time.time()
        run([py, swap, args.iso, '--restore'], out=subprocess.DEVNULL)
        if name != 'Mission_1A':
            run([py, swap, args.iso, '--redirect', slot, f'{LEVELS_DIR}/{name}.stream.zpm'], out=subprocess.DEVNULL)
        data = os.path.join(out, 'data')
        os.makedirs(data)
        shutil.copy(os.path.join(args.data_template, 'EBOOT.ELF'), data)
        ini = open(os.path.join(args.data_template, 'settings.ini')).read()
        ini = re.sub(r'(?m)^disc_image=.*$', 'disc_image=' + os.path.abspath(args.iso), ini)
        open(os.path.join(data, 'settings.ini'), 'w').write(ini)
        shutil.copytree(args.ms0, os.path.join(data, 'ms0'))
        env = dict(os.environ, INFECTED_DATA_DIR=data, INFECTED_NO_AUDIO='1', EGL_PLATFORM='surfaceless', LIBGL_ALWAYS_SOFTWARE='1',
                   INFECTED_LEAN_ONLY='1', INFECTED_LIBC_HOOKS='1', INFECTED_CLOCK='virtual', INFECTED_NO_BUSY_CLOCK='1', INFECTED_SPIN_INSNS='0',
                   INFECTED_FRAME_LIMIT_US='16683', INFECTED_UNTHROTTLED='1', INFECTED_PAD_SCRIPT='@' + os.path.abspath(args.script),
                   INFECTED_GL_DISPLAY_SCALE='1', INFECTED_TEX_DUMP=os.path.join(out, 'tex'), INFECTED_SCREENSHOT_DIR=os.path.join(out, 'shots'),
                   INFECTED_SCREENSHOT_EVERY='1500', INFECTED_MAX_FRAMES=str(args.max_frames), INFECTED_GE_DUMP=os.path.join(out, 'ge'),
                   INFECTED_GE_DUMP_FLIPS=','.join(str(f) for f in flips))
        with open(os.path.join(out, 'game.out'), 'w') as o, open(os.path.join(out, 'game.err'), 'w') as e:
            try:
                rc = run(['nice', '-n', '10', 'stdbuf', '-oL', args.game], env=env, timeout=args.seconds, out=o, err=e).returncode
            except subprocess.TimeoutExpired:
                rc = 'timeout'
        shutil.rmtree(data, ignore_errors=True)   # the memory stick copy
        result = {'level': name, 'exit': rc, 'game_seconds': round(time.time() - t0, 1)}
        # --- the pack against what was drawn
        keys = {}
        for f in glob.glob(os.path.join(out, 'tex', '*.json')):
            try:
                j = json.load(open(f))
                keys[j['key']] = j
            except Exception:
                pass
        miss = {m: [] for m in packs}
        direct = {m: 0 for m in packs}
        recol = {m: 0 for m in packs}
        for k, j in keys.items():
            for m, idx in packs.items():
                if int(k, 16) in idx:
                    direct[m] += 1
                elif j.get('image_key') and int(j['image_key'], 16) in idx and idx[int(j['image_key'], 16)]['flags'] & 1:
                    recol[m] += 1
                else:
                    t = by_key.get(k)
                    miss[m].append({'key': k, 'w': j['width'], 'h': j['height'], 'format': j['format'], 'names': t['names'][:2] if t else []})
        in_catalog = [by_key[k] for k in keys if k in by_key]
        result.update(textures=len(keys), in_catalog=len(in_catalog), direct_hits=direct, recolour_hits=recol, misses=miss['4x'],
                      misses_2x=miss['2x'], names_seen=sorted({t['names'][0] for t in in_catalog if t['names']})[:2000])
        # --- the recorded frames, replayed
        ge = os.path.join(out, 'ge')
        replay = {}
        if os.path.isdir(ge) and glob.glob(os.path.join(ge, 'flip_*.ge')):
            have = sorted(int(re.search(r'flip_(\d+)', f).group(1)) for f in glob.glob(os.path.join(ge, 'flip_*.ge')))
            for mode in ('off', '2x', '4x'):
                cmd = [args.replay, ge, '--flips', ','.join(str(f) for f in have), '--scale', '2', '--png',
                       os.path.join(out, 'frames', f'{mode}_{{flip}}.png'), '--stats', '--hd', mode, '--pack-dir', args.packs]
                env2 = dict(os.environ, EGL_PLATFORM='surfaceless', LIBGL_ALWAYS_SOFTWARE='1')
                try:
                    res = subprocess.run(['nice', '-n', '10'] + cmd, env=env2, capture_output=True, text=True, timeout=900)
                    text = res.stdout + '\n' + res.stderr
                except subprocess.TimeoutExpired:
                    text = 'timeout'
                replay[mode] = [l.strip() for l in text.split('\n') if 'hd textures' in l or 'texture working set' in l]
        result['replay'] = replay
        result['replayed_flips'] = sorted(int(re.search(r'flip_(\d+)', f).group(1)) for f in glob.glob(os.path.join(ge, 'flip_*.ge')))
        if not args.keep_ge:
            shutil.rmtree(ge, ignore_errors=True)
        # a texture whose palette the game rewrites (minimap, characters) is harvested once per palette: keep the pictures of the first three
        # variants of each image and only the json of the rest (a level can otherwise leave 700 MB of near identical PNGs)
        groups = {}
        for k, j in keys.items():
            groups.setdefault(j.get('image_key') or k, []).append(k)
        for ks in groups.values():
            for k in sorted(ks)[3:]:
                try:
                    os.remove(os.path.join(out, 'tex', k + '.png'))
                except OSError:
                    pass
        json.dump(result, open(os.path.join(out, 'result.json'), 'w'), indent=1)
        with open(os.path.join(args.out, 'coverage.tsv'), 'a') as f:
            f.write('\t'.join(str(x) for x in (name, rc, len(keys), len(in_catalog), direct['4x'], recol['4x'], len(miss['4x']), len(miss['2x']),
                                               ','.join(sorted({m['names'][0] if m['names'] else m['key'] for m in miss['4x']})))) + '\n')
        print(f'{name}: exit {rc}, {len(keys)} textures, direct {direct["4x"]}, recolour {recol["4x"]}, misses {len(miss["4x"])}, '
              f'{time.time() - t0:.0f}s', flush=True)
    run([py, swap, args.iso, '--restore'], out=subprocess.DEVNULL)
    return 0


if __name__ == '__main__':
    sys.exit(main())
