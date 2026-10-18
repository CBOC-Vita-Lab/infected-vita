# Install

Private preparation draft: current source/tool exports and a fresh-install
test are still needed before this becomes a release guide.

## You Need

- A homebrew-enabled PS Vita and VitaShell.
- Your own **Infected USA, `ULUS-10054`, version 1.02** ISO.
- The matching `infected.vpk`, application ID **`INFC00001`**.
- A legitimately obtained shader compiler module at
  `ur0:data/libshacccg.suprx`. It is not included. Use a documented lawful
  user-side setup; do not download an unofficial firmware/module bundle.

## Installation

1. Check the VPK against the release checksum and install it in VitaShell.
2. Copy your ISO to **`ux0:data/infected/Infected.iso`**.
3. Open the Infected bubble.

Use the filename above for the simplest documented path. The current code
is reported to discover other `.iso` names, but that variant has not been
verified, so this guide does not rely on it.

## Optional HD Textures

Normal installation does **not** require HDcube, model weights or an HD
download. To add textures generated from your own dump, see
[HD Textures](HD-TEXTURES.md). Put a compatible `texpack_2x.bin` or mixed
Vita `texpack_4x.bin` in `ux0:data/infected/`, not in the app folder.

The model runs on your computer. We have requested permission to include it;
once permission is granted and its conditions are met, we will add it to the
preparation package to make setup easier. This does not include permission
to redistribute the game's textures or movies.

## Update And Back Up

Exit the app, then back up **`ux0:data/infected/`**, particularly `ms0/`
(game saves) and `settings.txt`. Install the new VPK with the same
`INFC00001` ID. Keep the backup until the game starts, your settings remain
and a saved game loads. Do not delete saves or apply a test config as an
update step.

Removing the bubble is separate from deleting the data folder. Back up
before either operation; deleting the data folder removes the ISO, packs,
settings and saves stored there.

## Troubleshooting

| Boot Problem | Check |
| --- | --- |
| No disc image | Exact folder and `Infected.iso` filename; not a compressed archive. |
| Wrong executable | Use your supported USA 1.02 dump; renaming another revision will not fix it. |
| Missing shader compiler | Check the legitimate module setup and exact `ur0:` path above. |
| HD option unavailable | First run without HD, then check the compatible pack's name and location. |

Report the build and relevant error text, not your ISO, saves or an unreviewed
full log. Controls, the module setup procedure, RetroFlow copying and the
complete update path still need final source/install verification.
