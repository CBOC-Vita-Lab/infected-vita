#!/usr/bin/env python3
"""gen_profile.py: the PSVshellPlus profile that runs Infected (INFC00001) at 500 / 222 / 222 / 166 MHz. Installs nothing.

A PSVshellPlus profile is one 28-byte file named after the title id, kept in ur0:data/PSVshell/profiles/ on the Vita:
    char    ver[8]     "PSVS0300"
    uint32  lock       bit 0 CPU, 1 GPU, 2 bus, 3 GPU crossbar: the clocks the game may NOT change (its own scePower*Set*ClockFrequency is ignored)
    int32   clock[4]   CPU, GPU, bus, GPU crossbar in MHz
all little endian. The default (lock 0x0b = CPU + GPU + crossbar, clocks 500 / 222 / 222 / 166) is byte for byte the profile that was
measured on this Vita for another title (496 MHz measured, about 10 % less CPU time per frame than at 444).

  gen_profile.py [--out FILE] [--title INFC00001] [--cpu 500] [--gpu 222] [--bus 222] [--xbar 166] [--lock 0x0b]
  gen_profile.py --check FILE      print a profile's fields (to verify a file after a download)
"""
import argparse, os, struct, sys

def pack(cpu, gpu, bus, xbar, lock):
    return struct.pack('<8sI4i', b'PSVS0300', lock, cpu, gpu, bus, xbar)

def show(path):
    data = open(path, 'rb').read()
    if len(data) != 28:
        print('%s: %d bytes (a profile is 28)' % (path, len(data)))
        return 1
    ver, lock, cpu, gpu, bus, xbar = struct.unpack('<8sI4i', data)
    print('%s: ver %r lock 0x%X (locked: cpu %d gpu %d bus %d xbar %d) clocks cpu %d gpu %d bus %d xbar %d MHz' % (
        path, ver, lock, lock & 1, (lock >> 1) & 1, (lock >> 2) & 1, (lock >> 3) & 1, cpu, gpu, bus, xbar))
    return 0 if ver == b'PSVS0300' else 1

ap = argparse.ArgumentParser()
ap.add_argument('--title', default='INFC00001')
ap.add_argument('--out')
ap.add_argument('--cpu', type=int, default=500)
ap.add_argument('--gpu', type=int, default=222)
ap.add_argument('--bus', type=int, default=222)
ap.add_argument('--xbar', type=int, default=166)
ap.add_argument('--lock', type=lambda v: int(v, 0), default=0x0b)
ap.add_argument('--check')
a = ap.parse_args()
if a.check:
    sys.exit(show(a.check))
out = a.out or os.path.join(os.path.dirname(os.path.abspath(__file__)), a.title)
blob = pack(a.cpu, a.gpu, a.bus, a.xbar, a.lock)
assert len(blob) == 28
with open(out, 'wb') as f:
    f.write(blob)
sys.exit(show(out))
