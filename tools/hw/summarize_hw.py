#!/usr/bin/env python3
"""summarize_hw.py LOG... : one table of the console measurements in Infected logs (m_<case>.log from matrix.sh).

Per log: the bench's FINAL line (fps, 1% low), the averages of the renderer's [gl-time] lines (where the GL thread's
time goes per frame), the [stat] line's GE-thread share, and vitaGL's own profile when the build has it (time inside
draw calls, time waiting for the GPU, per 30 frames). The first gl-time block of a run (loading) is skipped.
"""
import re
import sys

NUM = r'([0-9.]+)'
GL = re.compile(r'\[gl-time\] per frame over \d+: target switches ' + NUM +
                r' \| first draw after a switch ' + NUM + r' ms \(' + NUM + r', worst ' + NUM + r' ms\)' +
                r' \| other draws ' + NUM + r' ms \(' + NUM + r'\)' +
                r' \| texture uploads ' + NUM + r' ms \(' + NUM + r'\)' +
                r'(?: \| read-backs ' + NUM + r' ms \(' + NUM + r'\))?' +
                r' \| present ' + NUM + r' ms \(swap ' + NUM + r'(?:, gpu finish ' + NUM + r')?')
FINAL = re.compile(r'\[play\] FINAL\s+(\d+)s flips=(\d+) fps=' + NUM + r' low1%=' + NUM)
REPLAY = re.compile(r'\[bench\] ge_replay total.*')
GE = re.compile(r'ge=(thr:\d+%|split:walk \d+% gl \d+%)')
PLAY = re.compile(r'\[play\] t=\s*(\d+)s flips=(\d+) fps=' + NUM + r' low1%=' + NUM + r' \| guest=' + NUM + r'ms ge=(\d+)% swap=' + NUM + r'ms \| draws/f=(\d+)')
VGL_FRAMES = re.compile(r'Last 30 frames took (\d+)ms')
VGL_DRAWS = re.compile(r'(\d+)ms spent processing (\d+) shaders pipeline draw calls')
VGL_STALL = re.compile(r'(\d+)ms spent waiting for GPU')


def mean(values):
    return sum(values) / len(values) if values else float('nan')


def summarize(path):
    gl, ge, finals, replay, play = [], [], [], [], []
    vgl_frames, vgl_draw_ms, vgl_draws, vgl_stall = [], [], [], []
    for line in open(path, errors='replace'):
        if m := GL.search(line):
            g = [float(x) if x is not None else 0.0 for x in m.groups()]
            gl.append({'switches': g[0], 'first_ms': g[1], 'other_ms': g[4], 'other_n': g[5], 'upload_ms': g[6],
                       'readback_ms': g[8], 'present_ms': g[10], 'swap_ms': g[11], 'finish_ms': g[12]})
        elif m := PLAY.search(line):
            play.append((int(m.group(1)), float(m.group(3)), float(m.group(4)), float(m.group(5)), int(m.group(6)),
                         float(m.group(7)), int(m.group(8))))
        elif m := FINAL.search(line):
            finals.append((int(m.group(1)), int(m.group(2)), float(m.group(3)), float(m.group(4))))
        elif m := REPLAY.search(line):
            replay.append(m.group(0)[:160])
        elif m := VGL_FRAMES.search(line):
            vgl_frames.append(int(m.group(1)))
        elif m := VGL_DRAWS.search(line):
            vgl_draw_ms.append(int(m.group(1)))
            vgl_draws.append(int(m.group(2)))
        elif m := VGL_STALL.search(line):
            vgl_stall.append(int(m.group(1)))
        if '[stat]' in line and (m := GE.search(line)):
            ge.append(m.group(1))
    out = [path]
    if finals:
        s, flips, fps, low = finals[-1]
        out.append(f'  FINAL {s}s flips={flips} fps={fps} low1%={low}')
    if len(play) > 1:
        body_play = play[1:]   # the first ten seconds include the mission's start
        out.append(f'  play ({len(body_play) * 10} s): fps {mean([p[1] for p in body_play]):.1f} (min {min(p[1] for p in body_play):.1f}), '
                   f'low1% {mean([p[2] for p in body_play]):.1f}, guest {mean([p[3] for p in body_play]):.1f} ms, '
                   f'GE thread {mean([p[4] for p in body_play]):.0f}%, swap {mean([p[5] for p in body_play]):.1f} ms, '
                   f'draws/f {mean([p[6] for p in body_play]):.0f}')
    for r in replay[-1:]:
        out.append('  ' + r)
    body = gl[1:] if len(gl) > 1 else gl
    if body:
        a = {k: mean([x[k] for x in body]) for k in body[0]}
        frame = a['first_ms'] + a['other_ms'] + a['upload_ms'] + a['readback_ms'] + a['present_ms']
        out.append(f"  GL thread per frame ({len(body)} blocks): draws {a['first_ms'] + a['other_ms']:.2f} ms "
                   f"({a['other_n']:.0f} draws, {a['switches']:.1f} target switches; first draw after a switch {a['first_ms']:.2f}) | "
                   f"uploads {a['upload_ms']:.2f} | read-backs {a['readback_ms']:.2f} | present {a['present_ms']:.2f} "
                   f"(swap {a['swap_ms']:.2f}, GPU finish {a['finish_ms']:.2f}) | sum {frame:.2f} ms")
    if ge:
        out.append(f'  GE share (last [stat] lines): {", ".join(ge[-3:])}')
    if vgl_frames:
        n = len(vgl_frames)
        out.append(f'  vitaGL per frame ({n} x 30 frames): frame {mean(vgl_frames) / 30:.2f} ms, inside draw calls '
                   f'{mean(vgl_draw_ms) / 30:.2f} ms ({mean(vgl_draws) / 30:.0f} draws), waiting for the GPU '
                   f'{mean(vgl_stall) / 30:.2f} ms')
    return '\n'.join(out)


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(__doc__.strip())
        sys.exit(2)
    for p in sys.argv[1:]:
        print(summarize(p))
