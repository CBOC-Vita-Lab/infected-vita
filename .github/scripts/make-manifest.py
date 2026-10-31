#!/usr/bin/env python3
"""Writes vita-install.json and SHA256SUMS for a release of the builder kits.

    python3 -I make-manifest.py KITS_DIR OUT_DIR

KITS_DIR holds the four Infected-Vita-builder-*.tar.gz files. The manifest follows the installer's release-format.md ("build"):
the kits are listed by <os>-<arch> with their SHA-256 and size, the player's disc is the input, and nothing made from the game is in
the release. Prints the manifest.
"""
import hashlib, json, os, sys

KITS = [  # installer key, file
    ('windows-amd64', 'Infected-Vita-builder-windows.tar.gz'),
    ('darwin-amd64', 'Infected-Vita-builder-mac-intel.tar.gz'),
    ('darwin-arm64', 'Infected-Vita-builder-mac-arm.tar.gz'),
    ('linux-amd64', 'Infected-Vita-builder-linux.tar.gz'),
]

def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()

def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    kits, out = sys.argv[1:3]
    kit = {}
    sums = []
    for key, name in KITS:
        p = os.path.join(kits, name)
        if not os.path.isfile(p):
            sys.exit('missing %s' % p)
        kit[key] = {'file': name, 'sha256': sha256(p), 'size': os.path.getsize(p)}
    manifest = {
        'name': 'Infected',
        'titleId': 'INFC00001',
        'build': {
            'from': 'disc',
            'kit': kit,
            'run': 'infected-build',
            'args': ['--progress', '--out', '{out}', '{in}'],
            'app': 'infected.vpk',
            'label': 'Building Infected from your disc',
            'minutes': 10,
        },
        'needs': [
            {'id': 'disc', 'type': 'psp-iso', 'gameId': 'ULUS-10054', 'to': 'ux0:data/infected/Infected.iso',
             'label': 'Your Infected UMD backup (PSP, USA, ISO)', 'prepare': [{'step': 'build'}]},
            {'id': 'hd-2x', 'type': 'file', 'to': 'ux0:data/infected/texpack_2x.bin', 'ext': ['.bin'], 'optional': True,
             'label': 'HD textures, 2x pack (optional)', 'names': ['texpack_2x.bin'], 'maxBytes': 120 << 20,
             'help': 'texpack_2x.bin, made from your own disc with the texture builder'},
            {'id': 'hd-4x', 'type': 'file', 'to': 'ux0:data/infected/texpack_4x.bin', 'ext': ['.bin'], 'optional': True,
             'label': 'HD textures, Vita 4x pack (optional)', 'names': ['texpack_4x_mixed.bin'], 'maxBytes': 160 << 20,
             'help': 'texpack_4x_mixed.bin (not the full 4x pack, which is too big for the Vita)'},
            {'id': 'shader', 'type': 'shader-compiler', 'optional': True},
        ],
    }
    os.makedirs(out, exist_ok=True)
    mpath = os.path.join(out, 'vita-install.json')
    with open(mpath, 'w') as f:
        json.dump(manifest, f, indent=2)
        f.write('\n')
    with open(os.path.join(out, 'SHA256SUMS'), 'w', newline='\n') as f:
        for _, name in KITS:
            f.write('%s  %s\n' % (kit[[k for k, n in KITS if n == name][0]]['sha256'], name))
        f.write('%s  vita-install.json\n' % sha256(mpath))
    print(open(mpath).read())

if __name__ == '__main__':
    main()
