import subprocess, sys, json, re, os
# usage: lean_measure.py <dir> <tag> [extra g++ flags...]
d, tag = sys.argv[1], sys.argv[2]; extra = sys.argv[3:]
env = dict(os.environ); env['PATH'] = '/home/claude/vitasdk-root/vitasdk/bin:' + env['PATH']
man = json.load(open(os.path.join(d, 'manifest.json')))
funcs = json.load(open(os.path.join(d, 'funcs.json')))
tot_text = 0; tot_sym = {}
for f in man['files']:
    base = os.path.basename(f['file'])
    obj = os.path.join(d, base.replace('.cpp', '_%s.o' % tag))
    cmd = ['nice', '-n', '10', 'arm-vita-eabi-g++', '-std=c++20', '-O2', '-g0', '-mcpu=cortex-a9', '-mfpu=neon', '-mfloat-abi=hard', '-mthumb',
           '-fno-exceptions', '-fno-rtti', '-fno-asynchronous-unwind-tables', '-I', d] + extra + ['-c', f['file'], '-o', obj]
    r = subprocess.run(cmd, env=env, capture_output=True, text=True)
    if r.returncode: print(r.stderr[:2000]); sys.exit(1)
    nm = subprocess.run(['arm-vita-eabi-nm', '-S', '--size-sort', obj], env=env, capture_output=True, text=True).stdout
    for l in nm.split('\n'):
        m = re.match(r'[0-9a-f]+ ([0-9a-f]+) [Tt] _Z\d+f_([0-9A-F]{8})', l)
        if m: tot_sym[int(m.group(2), 16)] = int(m.group(1), 16)
    sz = subprocess.run(['arm-vita-eabi-size', obj], env=env, capture_output=True, text=True).stdout.split('\n')[1].split()
    tot_text += int(sz[0])
words = man['words']; symbytes = sum(tot_sym.values())
fw = {int(k, 16): v['words'] for k, v in funcs.items()}
print('%s: functions=%d reachable_words=%d  sum(f_ symbol bytes)=%d  => %.2f B/word ; size(text incl rodata)=%d => %.2f B/word' % (
    tag, len(tot_sym), words, symbytes, symbytes / words, tot_text, tot_text / words))
json.dump({'%08X' % k: v for k, v in tot_sym.items()}, open(os.path.join(d, 'symsizes_%s.json' % tag), 'w'))
