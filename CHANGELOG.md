# What's New

## Version 1.1 — (unreleased)

**The install update.** Your own disc in, a finished game on your Vita out, in about 10 minutes, and smoother pictures with nothing to set.

**Easier install**
- The CBOC Vita Installer (version 2.0) now builds Infected on your own computer from your own disc. Pick the disc, press **Allow**, and it downloads a 40-50 MB builder once, builds the game in about 10 minutes (9 to 9.5 minutes on a slow 2-core PC; faster on most computers) and sends it to your Vita. Nothing is uploaded, and there is no terminal, Python or SDK.
- It runs on Windows 64-bit, macOS (10.13 and newer on Intel, 11 and newer on Apple silicon) and Linux 64-bit.
- It checks your disc the moment you pick it and says so plainly: wrong version, damaged copy, or a CSO/PBP file.
- **Prepare your Vita** installs ShaRKBR33D, which gets the optional shader compiler (it needs Wi-Fi on the Vita).
- Other computers (32-bit or ARM Windows, ARM Linux) can build on another machine and pick the finished `infected.vpk`.
- Not using the installer? The release has the same builder for each system, `infected-build`: drag your ISO onto it on Windows, or run `infected-build Infected.iso`, and it writes `infected.vpk`. Every system makes the byte-identical game.

**Smoother play**
- **Auto anti-aliasing is the new default.** FXAA is on while the GPU has room and switches off for heavy moments; as a last step the 3D scene is drawn at 85% for a few seconds while the HUD stays sharp. The menu shows **Auto (on)** or **Auto (off)**.
- **A lighter FXAA:** 5 to 7 screen reads instead of 9, and about half the shader maths. On a real Vita, mission 1 with FXAA and the 2x textures held 30 fps (give or take 1) for 164 of 181 seconds; 1.0's FXAA ran at 21 to 30, mostly in the mid-20s.
- The PSP's alpha test is skipped wherever it can't fail: draws covering 71% of the alpha-tested area, against 46% before, with a pixel-identical picture.
- Recoloured HD textures are stored compressed: 8 times less memory for them, and 2.4 times less HD texture memory overall in a mission-1 sample (7.8 to 3.3 MB).
- The latest vitaGL (2 November 2026).
- Players coming from 1.0 who never changed a setting move to Auto by themselves; anyone who changed settings keeps their anti-aliasing as it was.

**500 MHz**
- With the PSVshellPlus plugin installed, the game sets up its own 500 MHz profile and never overwrites one of yours. It takes effect from the next start.
- The CPU clock row shows the clock it measured (495 MHz on a real Vita, 1 November 2026).

**Fixes**
- The HD textures row no longer shows greyed out at start.
- Plain boot-screen messages if the shader compiler or memory is missing.
- Every one of the 18 menu rows and 46 values was tested.

Everything above is on by default for players who never open Settings. The full engineering record is in [docs/STATUS.md](docs/STATUS.md) and [vita/LOG.md](vita/LOG.md).

## Version 1.0 — October 23, 2026

**The first public version.** The whole game runs natively on the Vita, at a steady 30 fps.

**Smoother play**
- A steady 30 fps in mission 1 on the default settings, measured on a real PS Vita.
- All four of the Vita's CPU cores at work. The game unlocks the fourth one by itself, with nothing extra to install.
- Music and voices are decoded ahead of time on a spare core, leaving the game's own core free for the game.
- The game's code is built as one optimized program.
- Shaders are prepared during the opening logos, so the game doesn't pause the first time an effect appears.
- Effects the game has faded out completely are skipped instead of drawn.
- Frame skip: if the drawing falls behind, the action and the sound keep their pace.
- Optional 500 MHz CPU mode with a PSVshellPlus profile.

**Better picture**
- Full 960x544 rendering with smooth texture filtering and mipmaps.
- Optional HD texture packs (2x, or a mixed 4x) made from your own disc, switched live from the menu.
- Optional FXAA or MSAA anti-aliasing, per-pixel lighting and double-resolution glow effects.
- HUD icons and button glyphs are enlarged as pixel art instead of being blurred.

**Controls and gameplay**
- A settings menu that opens any time: hold SELECT + START, or tap the top-right corner of the screen.
- A Modern layout: R fires (hold for automatic fire), L locks on, and the right stick opens the map. Classic keeps the PSP layout.
- Optional higher enemy density (up to 200 %) and view distance (up to 200 %).

**Under the hood**
- Built from your own disc on a PC (1.1 does this for you in about 10 minutes). RetroFlow cover art is set up automatically, and you can use your own bubble art.

The full engineering record, with every measurement and its date, is in [docs/STATUS.md](docs/STATUS.md) and the logs in [docs/history/](docs/history).
