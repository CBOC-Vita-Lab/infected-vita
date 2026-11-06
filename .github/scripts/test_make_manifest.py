#!/usr/bin/env python3
"""Runs make-manifest.py on stand-in kit files and checks the manifest:  python3 -I .github/scripts/test_make_manifest.py"""
import json, os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
KITS = ['Infected-Vita-builder-windows.tar.gz', 'Infected-Vita-builder-mac-intel.tar.gz',
        'Infected-Vita-builder-mac-arm.tar.gz', 'Infected-Vita-builder-linux.tar.gz']

def main():
    with tempfile.TemporaryDirectory() as tmp:
        kits, out = os.path.join(tmp, 'kits'), os.path.join(tmp, 'out')
        os.makedirs(kits)
        for name in KITS:
            with open(os.path.join(kits, name), 'wb') as f:
                f.write(name.encode())
        subprocess.run([sys.executable, '-I', os.path.join(HERE, 'make-manifest.py'), kits, out], check=True, stdout=subprocess.DEVNULL)
        with open(os.path.join(out, 'vita-install.json')) as f:
            m = json.load(f)
        disc = next(n for n in m['needs'] if n['id'] == 'disc')
        assert disc['type'] == 'psp-iso' and disc['gameId'] == 'ULUS-10054', disc
        assert disc['check'] == {
            'file': 'PSP_GAME/SYSDIR/BOOT.BIN',
            'sha256': '952d4bb2c08209f4cbc0020d95c4cc601e9944cad5c7ade4c1d060fe8daab1bc',
            'message': 'This is not the US 1.02 version of Infected (ULUS-10054).'}, disc['check']
        assert 'requires' not in m['build'], 'build.requires must stay out: the kits run on every macOS the installer supports'
        assert sorted(m['build']['kit']) == ['darwin-amd64', 'darwin-arm64', 'linux-amd64', 'windows-amd64'], m['build']['kit']
        hd4 = next(n for n in m['needs'] if n['id'] == 'hd-4x')
        assert hd4['to'].endswith('texpack_4x.bin') and 'texpack_4x_mixed.bin' in hd4['names'], hd4
        sums = open(os.path.join(out, 'SHA256SUMS')).read().split('\n')
        assert len([l for l in sums if l]) == 5, sums
    print('make-manifest: ok')

if __name__ == '__main__':
    main()
