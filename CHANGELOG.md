# What's New

## Version 1.1 — (unreleased)

**Smoother and smoother-looking, with nothing to set**
- New anti-aliasing choice, **Auto**, now the default: the game turns on FXAA (a cheaper version than before) when the Vita's graphics chip has room and switches it off again the moment a scene needs the time, so edges are smoother without losing frames. The menu shows **Auto (on)** or **Auto (off)**.
- Players coming from 1.0 who never changed a setting move to Auto by themselves; anyone who changed settings keeps their anti-aliasing as it was.
- The log lists every speed feature the run has on (`[gl] speed features: ...`), and the settings menu was checked row by row, value by value (new automatic test).

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
- Builds from your own disc in about 20 minutes. RetroFlow cover art is set up automatically, and you can use your own bubble art.

The full engineering record, with every measurement and its date, is in [docs/STATUS.md](docs/STATUS.md) and the logs in [docs/history/](docs/history).
