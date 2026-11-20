#!/usr/bin/env python3
"""ab_report.py DIR : the table of an ab_session.sh run (DIR holds manifest.tsv and runs/N.log).

Per variant (a pair's A or B, all its rounds together): the frame rate per second of the bot's mission (the [stat] lines between
"[play] begin" and "[play] FINAL"): median, 5th percentile (p5: the rate 95 % of the seconds beat) and the 1 % low (the bench's own
figure from the FINAL line when the log has one, else the mean of the slowest 1 % of the seconds), plus the counters this round added:
the disc cache (ioc=), the present pacing (pace=, lock=), and what anti-aliasing Auto and its scene-scale rung did.
Per pair: B - A of the runs' median fps, round by round, its mean and a 95 % bootstrap interval over the rounds. The interval needs three
or more rounds; with fewer, the spread is shown and nothing is called.
"""
import csv
import os
import random
import re
import statistics
import sys

STAT = re.compile(r'\[stat\] +(?:[a-z]+ +)?t=(\d+)s flips=(\d+)\(\+(\d+)\)')
FINAL = re.compile(r'\[play\] FINAL\s+(\d+)s flips=(\d+) fps=([0-9.]+) low1%=([0-9.]+)')
IO_MS = re.compile(r'\bio=([0-9.]+) idle=')
IOC = re.compile(r'ioc=hit (\d+) miss (\d+) wait (\d+) pf (\d+) err (\d+)')
PACE = re.compile(r'pace=(\d+)/(\d+)/(\d+)/(\d+) lock=(\d)')
AA = re.compile(r'aa=auto:(on|off)')
SS = re.compile(r'ss=(scaled|full)\(')


def percentile(values, p):
    """Rank p (0..100) of the sorted values, linear between neighbours."""
    if not values:
        return float('nan')
    v = sorted(values)
    k = (len(v) - 1) * p / 100.0
    lo = int(k)
    hi = min(lo + 1, len(v) - 1)
    return v[lo] + (v[hi] - v[lo]) * (k - lo)


def low_one(values):
    if not values:
        return float('nan')
    worst = sorted(values)[:max(1, len(values) // 100)]
    return statistics.mean(worst)


def parse_log(path):
    """The mission's per-second rates and counters of one log; None when the log has no mission in it."""
    run = {'fps': [], 'final_low1': None, 'final_fps': None, 'io_ms': [], 'ioc': [0, 0, 0, 0, 0], 'pace': [0, 0, 0, 0], 'lock': None,
           'aa_on': 0, 'aa_n': 0, 'ss_scaled': 0, 'ss_n': 0, 'clean': False}
    inside = False
    prev_t = None
    try:
        lines = open(path, errors='replace').read().splitlines()
    except OSError:
        return None
    for line in lines:
        if 'clean exit' in line:
            run['clean'] = True
        if '[play] begin' in line:
            inside = True
            prev_t = None
            continue
        m = FINAL.search(line)
        if m:
            run['final_fps'] = float(m.group(3))
            run['final_low1'] = float(m.group(4))
            inside = False
            continue
        if not inside:
            continue
        m = STAT.search(line)
        if not m:
            continue
        t, df = int(m.group(1)), int(m.group(3))
        if prev_t is not None and t > prev_t:
            run['fps'].append(df / (t - prev_t))
        prev_t = t
        if (m := IO_MS.search(line)):
            run['io_ms'].append(float(m.group(1)))
        if (m := IOC.search(line)):
            for i in range(5):
                run['ioc'][i] += int(m.group(i + 1))
        if (m := PACE.search(line)):
            for i in range(4):
                run['pace'][i] += int(m.group(i + 1))
            run['lock'] = int(m.group(5))
        if (m := AA.search(line)):
            run['aa_n'] += 1
            run['aa_on'] += m.group(1) == 'on'
        if (m := SS.search(line)):
            run['ss_n'] += 1
            run['ss_scaled'] += m.group(1) == 'scaled'
    return run if run['fps'] else None


def bootstrap_ci(diffs, n=4000, seed=1):
    rng = random.Random(seed)
    means = sorted(statistics.mean(rng.choice(diffs) for _ in diffs) for _ in range(n))
    return means[int(n * 0.025)], means[int(n * 0.975) - 1]


def fmt(x, nd=1):
    return 'n/a' if x != x else f'{x:.{nd}f}'


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    base = argv[1]
    rows = list(csv.DictReader(open(os.path.join(base, 'manifest.tsv')), delimiter='\t'))
    runs = {}      # (pair, side) -> [run dicts]
    by_round = {}  # (pair, round) -> {side: median fps}
    skipped = []
    order = []
    for r in rows:
        key = (r['pair'], r['side'])
        if key not in runs:
            runs[key] = []
            order.append(key)
        if r['status'] != 'ok':
            skipped.append(f"run {r['run']} ({r['pair']} {r['side']}): {r['status']}")
            continue
        parsed = parse_log(os.path.join(base, 'runs', r['log']))
        if parsed is None:
            skipped.append(f"run {r['run']} ({r['pair']} {r['side']}): no mission in the log (the bot did not get into mission 1?)")
            continue
        parsed['median'] = statistics.median(parsed['fps'])
        runs[key].append(parsed)
        by_round.setdefault((r['pair'], r['round']), {})[r['side']] = parsed['median']

    print('A/B session report: %s' % base)
    print('fps = frames shown per second of the mission (per-second [stat] lines); p5 = rate the slowest 5 % of the seconds had; 1% low = the bench figure')
    print()
    head = f"{'variant':<30}{'runs':>5}{'secs':>7}{'fps med':>9}{'p5':>7}{'1% low':>8}{'io ms':>7}{'cache hit%':>11}{'pace 1/2/3/4+ %':>20}{'lock':>5}{'aa on%':>8}{'scaled%':>9}"
    print(head)
    print('-' * len(head))
    for key in order:
        rs = runs[key]
        label = f'{key[0]} {key[1]}'
        if not rs:
            print(f'{label:<30}{0:>5}   (no usable run)')
            continue
        pooled = [x for r in rs for x in r['fps']]
        lows = [r['final_low1'] for r in rs if r['final_low1'] is not None]
        low = statistics.mean(lows) if lows else low_one(pooled)
        io_ms = [x for r in rs for x in r['io_ms']]
        ioc = [sum(r['ioc'][i] for r in rs) for i in range(5)]
        blocks = ioc[0] + ioc[1] + ioc[2]
        hit = '%5.1f' % (100.0 * (ioc[0] + ioc[2]) / blocks) if blocks else '  n/a'
        pace = [sum(r['pace'][i] for r in rs) for i in range(4)]
        ptotal = sum(pace)
        pace_s = '/'.join('%.0f' % (100.0 * p / ptotal) for p in pace) if ptotal else 'n/a'
        lock = next((r['lock'] for r in rs if r['lock'] is not None), None)
        aa_n = sum(r['aa_n'] for r in rs)
        ss_n = sum(r['ss_n'] for r in rs)
        aa = '%.0f' % (100.0 * sum(r['aa_on'] for r in rs) / aa_n) if aa_n else 'n/a'
        ss = '%.0f' % (100.0 * sum(r['ss_scaled'] for r in rs) / ss_n) if ss_n else 'n/a'
        print(f"{label:<30}{len(rs):>5}{len(pooled):>7}{fmt(statistics.median(pooled)):>9}{fmt(percentile(pooled, 5)):>7}{fmt(low):>8}"
              f"{fmt(statistics.mean(io_ms), 2) if io_ms else 'n/a':>7}{hit:>11}{pace_s:>20}{('n/a' if lock is None else lock):>5}{aa:>8}{ss:>9}")
    print()
    print('Pairs: B - A of the median fps of each round (the run-to-run noise is about 1 fps, so one round proves nothing)')
    pairs = []
    for key in order:
        if key[0] not in pairs:
            pairs.append(key[0])
    for p in pairs:
        diffs = []
        for (name, rnd), sides in sorted(by_round.items()):
            if name == p and 'A' in sides and 'B' in sides:
                diffs.append(sides['B'] - sides['A'])
        if not diffs:
            print(f'  {p:<26} no complete A/B round')
            continue
        mean = statistics.mean(diffs)
        per_round = ' '.join('%+.1f' % d for d in diffs)
        if len(diffs) >= 3:
            lo, hi = bootstrap_ci(diffs)
            verdict = 'B faster' if lo > 0 else ('B slower' if hi < 0 else 'no difference shown')
            print(f'  {p:<26} {mean:+.2f} fps  95% interval [{lo:+.2f}, {hi:+.2f}]  rounds: {per_round}  -> {verdict}')
        else:
            print(f'  {p:<26} {mean:+.2f} fps  rounds: {per_round}  (fewer than 3 rounds: no interval)')
    if skipped:
        print()
        print('Not used:')
        for s in skipped:
            print('  ' + s)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
