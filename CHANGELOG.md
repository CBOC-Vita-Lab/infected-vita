# What's New

## Version 1.0 — October 23, 2026

**The first public version.** The whole game runs natively on the Vita, at a steady 30 fps.

**Smoother play**
- A steady 30 fps in mission 1 on the default settings, measured on a real PS Vita.
- The game now uses all four of the Vita's CPU cores. It unlocks the fourth one by itself, with nothing extra to install.
- Music and voices are decoded ahead of time on a spare core, which frees about 3 ms of every frame on the game's own core.
- The game's code is built as one optimized program, so it runs faster.
- Shaders are prepared during the opening logos, so the game doesn't pause the first time an effect appears.
- Effects the game has faded out completely are no longer drawn.
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
