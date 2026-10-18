# Install

For the desktop app and matching ZIP, see [Desktop Install](DESKTOP-INSTALL.md).
The manual method below remains available. Private preview access is required.

Use your owner-built `INFC00001` VPK or your existing private-preview bundle.
[Start Here](START-HERE.txt) is the short version.

## You Need

- A homebrew-enabled PS Vita and VitaShell.
- Your own **Infected USA, `ULUS-10054`, version 1.02** ISO.
- Your [owner-built VPK](BUILDING.md), application ID **`INFC00001`**.
  The ready-made private candidate is for the original owner's use only.
- A legitimately obtained shader compiler module at
  `ur0:data/libshacccg.suprx`. It is not included.

## Installation

1. Build `dist/infected.vpk`, or use your existing private install bundle.
   If using that bundle, extract the ZIP and [check it](CHECK-DOWNLOAD.md).
   Leave the VPK intact. Copy it through VitaShell USB or FTP to `ux0:VPK/`
   (create that folder if needed), then end the transfer. In VitaShell,
   highlight the VPK, use the confirm button (usually Cross), and confirm
   installation.
2. Copy your ISO to **`ux0:data/infected/Infected.iso`**.
3. Open the Infected bubble.

Use the filename above to avoid folder and naming problems.

```text
ux0:data/infected/
  Infected.iso          your supported disc image
  ms0/                  keep existing game saves
  settings.txt          keep existing settings
  texpack_2x.bin         optional compatible user-generated pack
  texpack_4x.bin         optional mixed Vita pack, not the all-4x pack
```

The app creates its own save/settings files; do not download starter saves
or copy these labels as empty files. Do not nest another `infected/` folder.

## Optional HD Textures

Normal installation does **not** require HDcube, model weights or an HD
download. To add textures generated from your own dump, see
[HD Textures](HD-TEXTURES.md). Put a compatible `texpack_2x.bin` or mixed
Vita `texpack_4x.bin` in `ux0:data/infected/`, not in the app folder.

The included builder downloads the model from its author. See the HD guide
for the one-command preparation and optional manual setup.

## Update And Back Up

| Item | What To Do |
| --- | --- |
| Matching `INFC00001` VPK | Install with VitaShell; never copy individual app files by hand. |
| `Infected.iso` | Keep your existing supported ISO; an app update does not need another copy. |
| `ms0/` and `settings.txt` | Keep and back up; never replace with a test profile. |
| Optional texture packs | Keep. Replace only for an intentional compatible pack update, after backup. |
| `libshacccg.suprx` | Keep your legitimate module in `ur0:data/`; not part of this update. |

If a computer copy operation asks to replace the whole data folder, cancel
and check the destination. Do not use automatic folder synchronization.

Exit the app, then back up **`ux0:data/infected/`**, particularly `ms0/`
(game saves) and `settings.txt`. Install the new VPK with the same
`INFC00001` ID. Keep the backup until the game starts, your settings remain
and a saved game loads. Do not delete saves or apply a test config as an
update step.

This runtime has a one-time migration from an older 444 MHz setting to a
500 MHz request. Check the CPU clock option after updating and preserve
`cpu_clock_500.done` with your backup. See [Settings](docs/SETTINGS.md).
No kernel-plugin or clock-profile installation is part of these instructions.

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
full log. [Controls](CONTROLS.md) are checked against the recovered source.
Build-specific notes are in [Known Issues](KNOWN-ISSUES.md).
