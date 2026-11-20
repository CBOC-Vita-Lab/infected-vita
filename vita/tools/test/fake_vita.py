#!/usr/bin/env python3
"""A pretend console for the A/B runner's tests: vitacompanion's FTP server (port 1337) and command server (port 1338) in one process,
and a pretend game. The "game" starts when it gets `launch`: it reads config.txt from the FTP store and, a moment later, puts a log.txt
there that looks like the real one (config dump, a mission, [stat] lines, FINAL, clean exit) with frame rates that depend on the config:
io_cache=1 adds 1.5 fps, present_vblank_lock=1 adds 0.5, gl_scene_scale=2 takes 3 away; every run has noise.
usage: fake_vita.py FTP_PORT CMD_PORT STORE_DIR   (prints "ready" when both listen). Forbidden paths (sce_sys, ms0) are logged to STORE_DIR/violations.txt."""
import os
import random
import socketserver
import sys
import threading
import time

ftp_port, cmd_port, store = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
os.makedirs(store, exist_ok=True)
lock = threading.Lock()
files = {}   # "ux0:/data/infected/config.txt" -> bytes
files['ux0:/data/infected/config.txt'] = b'overlay=gpu\nhud=0\n'
files['ux0:/data/infected/settings.txt'] = b'gfx.antialiasing=auto\n'
running = {'label': None}
rng = random.Random(4)


def violation(path):
    if 'sce_sys' in path or '/ms0' in path or 'savedata' in path.lower() or not path.startswith('ux0:/data/infected/'):
        with open(os.path.join(store, 'violations.txt'), 'a') as f:
            f.write(path + '\n')


def norm(cwd, arg):
    if ':' in arg.split('/')[0]:
        parts = arg.split('/')
    else:
        parts = cwd.split('/') + arg.split('/')
    out = []
    for p in parts:
        if p in ('', '.'):
            continue
        if p == '..':
            out and out.pop()
            continue
        out.append(p)
    if out and out[0].endswith(':'):
        return out[0] + '/' + '/'.join(out[1:]) if len(out) > 1 else out[0] + '/'
    return '/'.join(out)


class Ftp(socketserver.StreamRequestHandler):
    def reply(self, text):
        self.wfile.write((text + '\r\n').encode())

    def handle(self):
        cwd = ''
        data_srv = None
        self.reply('220 fake vitacompanion')
        for raw in self.rfile:
            line = raw.decode(errors='replace').strip()
            cmd, _, arg = line.partition(' ')
            cmd = cmd.upper()
            if cmd == 'USER':
                self.reply('331 ok')
            elif cmd == 'PASS':
                self.reply('230 ok')
            elif cmd == 'PWD':
                self.reply('257 "/"')
            elif cmd in ('TYPE', 'MODE', 'STRU', 'NOOP'):
                self.reply('200 ok')
            elif cmd == 'CWD':
                cwd = norm(cwd, arg)
                self.reply('250 ok')
            elif cmd == 'PASV':
                data_srv = socketserver.TCPServer(('127.0.0.1', 0), socketserver.BaseRequestHandler, bind_and_activate=True)
                port = data_srv.server_address[1]
                self.reply('227 Entering Passive Mode (127,0,0,1,%d,%d)' % (port >> 8, port & 255))
            elif cmd == 'EPSV':
                self.reply('502 no')
            elif cmd == 'SIZE':
                path = norm(cwd, arg)
                with lock:
                    if path in files:
                        self.reply('213 %d' % len(files[path]))
                    else:
                        self.reply('550 no such file')
            elif cmd == 'RETR':
                path = norm(cwd, arg)
                violation(path)
                with lock:
                    blob = files.get(path)
                if blob is None or data_srv is None:
                    self.reply('550 no such file')
                    continue
                self.reply('150 opening')
                conn, _ = data_srv.get_request()
                conn.sendall(blob)
                conn.close()
                data_srv.server_close()
                data_srv = None
                self.reply('226 done')
            elif cmd == 'STOR':
                path = norm(cwd, arg)
                violation(path)
                self.reply('150 opening')
                conn, _ = data_srv.get_request()
                chunks = []
                while True:
                    b = conn.recv(65536)
                    if not b:
                        break
                    chunks.append(b)
                conn.close()
                data_srv.server_close()
                data_srv = None
                with lock:
                    files[path] = b''.join(chunks)
                self.reply('226 done')
            elif cmd == 'DELE':
                path = norm(cwd, arg)
                violation(path)
                with lock:
                    existed = files.pop(path, None) is not None
                self.reply('250 deleted' if existed else '550 no such file')
            elif cmd == 'QUIT':
                self.reply('221 bye')
                return
            else:
                self.reply('502 not implemented')


def pretend_game(label_cfg):
    cfg = {}
    for line in label_cfg.decode().splitlines():
        if '=' in line:
            k, v = line.split('=', 1)
            cfg[k.strip()] = v.strip()
    base = 30.0 + (1.5 if cfg.get('io_cache') != '0' and 'io_cache' in cfg else 0.0)
    if cfg.get('present_vblank_lock') == '1':
        base += 0.5
    if cfg.get('gl_scene_scale') == '2':
        base -= 3.0
    if cfg.get('gfx.antialiasing') == 'fxaa':
        base -= 1.0
    secs = int(cfg.get('bench_seconds', '20'))
    run_bias = rng.gauss(0, 0.6)
    out = ['Infected for PS Vita | built fake', 'config:'] + ['INFECTED_%s=%s' % (k.upper(), v) for k, v in cfg.items()]
    out.append('[play] begin at flip 100: a mission is running, the bot plays; a line every 10s, the run ends %ds after this point' % secs)
    t = 40
    hits = 0
    for i in range(secs):
        fps = max(5.0, base + run_bias + rng.gauss(0, 1.5) - (6 if rng.random() < 0.02 else 0))
        df = int(round(fps))
        hit = rng.randint(20, 40) if cfg.get('io_cache') != '0' else 0
        miss = rng.randint(0, 3) if cfg.get('io_cache') != '0' else 0
        io = ' | ioc=hit %d miss %d wait 0 pf %d err 0' % (hit, miss, hit // 2) if cfg.get('io_cache') != '0' else ''
        lock = 2 if cfg.get('present_vblank_lock') == '1' else 1
        p2 = df if lock == 2 else int(df * 0.9)
        out.append('[stat]  t=%ds flips=%d(+%d) presented=%d(+%d) frame=33.3ms ge=split:walk 60%% gl 70%% | ms/frame guest=9.0 hle=off gewalk=5.0 (vertex=1.0) '
                   'glsubmit=3.0 present=2.0 decode=0.0 audio=0.4 io=%.1f idle=1.0 (gesync=0.0 flipwait=0.0) | splitwait=0.0%s | pace=0/%d/%d/0 lock=%d'
                   % (t, 1000 + i * 30, df, 1000 + i * 30, df, rng.random() * 0.5, io, p2, df - p2, lock))
        t += 1
        hits += hit
    out.append('[play] FINAL %4ds flips=%d fps=%.1f low1%%=%.1f | guest=9.0ms' % (secs, secs * 30, base + run_bias, base + run_bias - 8))
    out.append('runtime stopped: bench complete (play)')
    out.append('clean exit')
    return '\n'.join(out).encode() + b'\n'


class Cmd(socketserver.StreamRequestHandler):
    def handle(self):
        for raw in self.rfile:
            line = raw.decode(errors='replace').strip()
            with open(os.path.join(store, 'commands.txt'), 'a') as f:
                f.write(line + '\n')
            if line.startswith('launch'):
                with lock:
                    cfg = files.get('ux0:/data/infected/config.txt', b'')
                def later():
                    time.sleep(0.4)
                    with lock:
                        files['ux0:/data/infected/log.txt'] = pretend_game(cfg)
                threading.Thread(target=later, daemon=True).start()
            self.wfile.write(b'ok\n')


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


for port, handler in ((ftp_port, Ftp), (cmd_port, Cmd)):
    threading.Thread(target=Server(('127.0.0.1', port), handler).serve_forever, daemon=True).start()
print('ready', flush=True)
while True:
    time.sleep(1)
    with lock:
        for path, blob in files.items():
            with open(os.path.join(store, path.replace(':', '_').replace('/', '__')), 'wb') as f:
                f.write(blob)
