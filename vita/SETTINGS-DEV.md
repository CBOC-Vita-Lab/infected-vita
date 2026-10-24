# Infected for PS Vita: the settings menu (developer notes)

The shorter, player-facing version of this page is `SETTINGS.md`. This one keeps the measurements and the reasoning (pack memory, palette watch, what was checked).
The enemy density and view distance rows are live since the tuning merge; HD movies is EXPERIMENTAL (greyed until `movies_hd/` is installed).

Everything the port lets you change lives in one in-game menu. It works while you play: open it, change a value, close it, and the
game goes on from the same moment. (Checked in the Vita3K emulator; nothing here has run on a real console yet, so the "cost" column
is an estimate, not a measurement.)

## Opening it

* **Hold SELECT + START for half a second**, or **tap the top-right corner of the touch screen** (the corner is about a sixth of the
  screen wide and a fifth high; the tap is a button, it does nothing else).
* SELECT + START together with L or R is *not* the menu: START + SELECT + L + R is the way out of the game.
* While the menu is open the game is **paused**: its clock stands still (the mission timer does not run), the sound is paused, and the
  game does not see any button. When you close the menu the clock carries on from where it stopped; nothing is skipped.

| Button | In the menu |
| --- | --- |
| UP / DOWN | choose a row |
| LEFT / RIGHT | change the value of the row |
| L / R | switch page (Graphics, Performance) |
| CIRCLE or START | close the menu and save |

A row that is **greyed** cannot be used on this build (the reason is shown under the list). A value marked **restart needed** is saved
and takes effect the next time the game starts. The colour of the "cost" column is a guess of what the value costs the Vita:
None, Low, Medium, High.

The choices are kept in `ux0:data/infected/settings.txt` (one `key=value` per line, written when you close the menu). Delete the file
to go back to the defaults. A line in `config.txt` with the same key (`gfx.texture_filter=original`, dotted) is a developer override:
it decides for the whole run, the menu shows it and cannot change it, and it is never written to `settings.txt`.

## Graphics page

| Option (key) | Values (default first) | What it does on the Vita | Cost |
| --- | --- | --- | --- |
| **HD textures** `gfx.hd_textures` | Automatic, Off, 2x pack, 4x pack | Replaces the game's textures by the sharper ones in `texpack_2x.bin` / `texpack_4x.bin` (see "HD texture packs"). **Automatic** takes the 2x pack if it is installed, else the 4x pack (why: see "Which pack the Vita uses"). Greyed with "No texture pack installed." when neither file is in `ux0:data/infected/`. Changing it applies at once: the textures in use are dropped and loaded again from the chosen pack (or the disc) within a few seconds. The "4x pack" on the Vita is the *mixed* pack (4x world and effects, 2x characters and menus) installed under the name `texpack_4x.bin`. | 2x low, 4x medium |
| **Texture filter** `gfx.texture_filter` | Smooth, Original | Smooth: the textures of the 3D world that the game draws with the PSP's blocky (nearest) filter are filtered bilinearly instead. Left alone: textures with cut-out holes or colour keys (they would bleed at the edges), tiny ones, atlas parts, and the HUD, menus and text. Original: exactly what the PSP game asks for. | Smooth low, Original none |
| **Mipmaps** `gfx.mipmaps` | On, Off | Smaller copies of the 3D textures that are filtered, for distant ground and walls (less shimmer). They are built when a texture is loaded and take about a third more texture memory. Off: only the full size. | On low |
| **Anti-aliasing** `gfx.antialiasing` | Off, FXAA, MSAA 2x, MSAA 4x | FXAA is a filter over the finished 3D picture (not over the HUD) and takes effect at once. MSAA is the GPU's own multisampling: the display is set up with it when the game starts, so changing to or from MSAA **needs a restart**. | FXAA medium, MSAA 2x medium, MSAA 4x high |
| **Pixel lighting** `gfx.pixel_lighting` | Off, On | Characters are lit per pixel instead of per vertex (smoother highlights). The heaviest option here. | On high |
| **Effects resolution** `gfx.effects_scale` | Normal, Double | Resolution of the offscreen passes behind the glow and blur effects (Double = twice the width and height). | Double medium |
| **Movie scaling** `gfx.movie_filter` | Sharp, Bilinear | How the 480x272 movies are enlarged to the screen: Sharp keeps hard pixel edges, Bilinear smooths them. | Sharp low |
| **HD movies** `gfx.hd_movies` | Off, On | The 18 movies are replaced by 960x544 versions made offline (`tools/hdmovies/build_movies.sh`, see `docs/HD_MOVIES.md`). The game's own movie player, sound and skip are unchanged; only the picture is. **Greyed ("No replacement movies yet.") until `ux0:data/infected/movies_hd/index.txt` lists installed movies.** A movie without a replacement plays as before. Uses the hardware H.264 decoder (unverified on a real Vita). | Off |

## Performance page

| Option (key) | Values (default first) | What it does on the Vita | Cost |
| --- | --- | --- | --- |
| **Frame limit** `perf.frame_cap` | 60 fps, 30 fps, Off | The most frames shown per second. The game does not limit itself (it flips as fast as it can compute), so 60 really is a ceiling of 60 and 30 halves the work and the heat. **Off** removes the limit and also the wait for the display's vertical sync, so the picture can tear; use it to see what the Vita can do. Applies at once. | none |
| **Frame skip** `perf.frame_skip` | Auto, Off | With the split renderer: while the GL thread runs more than about 14 ms behind the walker, GE jobs count as completed once the walker is through them (the guest is not held for the drawing; the interrupts keep their place in the GL queue), and while it is more than about 28 ms behind, the next frame's draws, read-backs, uploads and present are left out (at most two frames in a row). Back to exact completion when the lag falls under 5 ms. Costs a texture rewritten by the game early (it shows a frame early) while it runs ahead, nothing otherwise. Developer override `INFECTED_FRAME_SKIP=off|auto|always`, thresholds `INFECTED_FRAME_SKIP_ENTER_US/_LEAVE_US/_LAG_US/_STREAK`, test aid `INFECTED_GE_SPLIT_SLOW_PRESENT_US`. The stat line shows `skip=<frames> catchup=<0/1> lag=<ms>`. | none |
| **Show FPS** `perf.show_fps` | Off, On | A counter in the top-left corner: frames per second, milliseconds per frame, draw calls. | On low |
| **Resolution** `perf.resolution` | 960x544, 720x408 | The size the game is drawn at. 720x408 draws 56 % of the pixels and is enlarged to the screen: softer, and faster when the GPU is the limit. **Needs a restart.** | none |
| **Renderer thread** `perf.threaded_renderer` | On, Off, Split | On: the game runs on one CPU core and the drawing on another (the default). Split: the drawing itself is split over two more cores, one walking the display lists and decoding vertices, one making the GL calls (the most parallel; new). Off: everything on one core (for tracking a problem). **Needs a restart.** | none |
| **CPU clock** `perf.cpu_clock` | 444 MHz, 500 MHz | The Vita's CPU speed. 444 MHz is the highest the retail firmware allows; 500 MHz (the Vita build's default) works only with a plugin that lifts the limit: on the test console that is PSVshellPlus with the profile in `psvshell/` (README there), which also makes the system keep answering 444, so the log's `clocks: the ARM core measures N MHz` line is the one to read. If the system refuses 500 the clock stays at 444 and `log.txt` says so (`settings: the ARM clock stays at ...`). Higher runs hotter and drains the battery faster. The GPU (222 MHz) and bus (222 MHz) clocks are always set at start, with the crossbar at 166; `config.txt` `clocks=0` leaves all of them alone. A `settings.txt` written before 500 became the default holds `perf.cpu_clock=444` (the menu writes every row); the first start of this build turns that into 500 once (marker file `cpu_clock_500.done`, log line `settings: perf.cpu_clock 444 ... is now 500 MHz`), and a 444 chosen later stays. | 500 low |
| **Enemy density** `perf.enemy_density` | 100%, 125%, 150%, 200% | How many enemies may be alive at the same time in each area of a mission, as a percentage of what the level allows (mission 1: 4 at 100%, 8 at 200%). **Applies at the next mission** (when a mission loads, also a retry after dying); the mission you are in keeps its number. Lowering it back works the same way. Only the number on the street changes: how many enemies a level has in all, the objectives, the timers and the medals are the game's own, so the same kills are asked for, just more of them at once. If the game has too little memory left (under 1 MB free) the number is not raised. | 125% medium, 150% high, 200% high |
| **View distance** `perf.view_distance` | 100%, 150%, 200% | How far the world is drawn before the fog hides it: the far plane and the fog start of the level's areas are multiplied. Applies at once while you play. In levels whose world reaches beyond the original distance (the big park levels and the long avenues) more of the skyline stays visible; in a closed level (mission 1: a street that ends in fire and buildings) it mostly thins the haze. The game does not skip anything beyond the far plane by itself, so the processor and the number of draw calls do not change; the cost is on the GPU (more is clipped and filled) and has **not been measured**. | 150% medium, 200% medium |

## How the texture options fit together

The order a texture goes through is: the game's own filter choice, then **Texture filter** and **Mipmaps**, then the **HD texture**
replacement, then the sharpening of the final picture.

* **HD pack textures keep the mip levels that are stored in the pack** (they were made with the pack, they are better than ones made
  on the console). Turning **Mipmaps** off uses only the full-size level of the HD texture, filtered bilinearly. Turning it on does
  not generate anything for a texture that came from a pack.
* **Texture filter = Original with an HD pack:** the pack's textures are still *shrunk* with a linear filter (a nearest-neighbour
  filter would shimmer badly at four times the resolution); when a texture is *enlarged* the game's own choice (blocky or smooth)
  decides, as it does without a pack.
* HD textures, mipmaps and the filter are all cheap compared with **Pixel lighting** and **MSAA 4x**; start there if the game
  runs slowly.

## HD texture packs

The port loads two optional files from `ux0:data/infected/`. They are made by a separate tool from the game's own images, are **not part of
the VPK**, and are for your own copy of the game only:

| File on the Vita | Size | What it is |
| --- | --- | --- |
| `texpack_2x.bin` | about 72 MB (72,213,209 bytes) | every one of the 2,625 replaced textures at twice the original size. The pack of the menu choice **2x pack**. |
| `texpack_4x.bin` | about 102 MB (102,362,489 bytes) | the **mixed 4x pack**: world, props and effects at four times the original size, characters and menus at twice. The pack of the menu choice **4x pack**. |

The repository's pack folder has the mixed pack under the name `texpack_4x_mixed.bin`: **rename it to `texpack_4x.bin`** when you copy it
(`curl -T texpack_4x_mixed.bin ftp://<vita-ip>:1337/ux0:/data/infected/texpack_4x.bin`). There is also a full 4x pack (every texture at
four times, 274 MB); it is meant for the desktop and is **too big for the Vita's texture memory**: the port logs a warning
(`[hdtex] texpack_4x.bin is N MB: that is the full 4x pack, which does not fit the Vita ...`) if the file called `texpack_4x.bin` is larger than 160 MB. Nothing else has to be done after copying: the menu row
"HD textures" stops being greyed. (Format of all packs: IFTP version 1, unchanged.)

Most textures are stored compressed (DXT1 and DXT5, which the Vita's GPU reads directly at 4 or 8 bits per pixel), a smaller part
as plain 32-bit images. Textures with a changing colour table (the glowing street lights, fires and the radar) are stored once and recoloured when the game changes the colours.

### Which pack the Vita uses, and how much memory it gets

* **Automatic = the 2x pack if `texpack_2x.bin` is installed, otherwise the 4x pack.** The rule is the same on the desktop and in the tests. It is 2x because nothing about the
  real console is known yet: 2x costs a quarter of the texture memory (about 18-21 MB of a mission-1 level against 26-37 MB for the mixed 4x pack, and 71-87 MB for a full 4x pack), so
  it is the choice that is least likely to run out of memory or make the frame rate worse. Once the first real-Vita numbers are in (`docs/history/release-candidate/HARDWARE-TEST.md`), Automatic can pick the 4x pack when they allow it; choosing "4x pack" yourself is always possible.
* **How much GPU memory the replacements may use** (the rest of the textures keep their originals; the numbers include the textures the game draws at that moment):

  | Mode | Default budget | `config.txt` key |
  | --- | --- | --- |
  | 2x pack | 32 MB | `hd_budget_2x_mb` |
  | 4x pack (mixed) | 48 MB | `hd_budget_4x_mb` |
  | both (overrides the two above) | | `hd_budget_mb` |

  When the budget is spent, textures that do not fit keep the original (nothing breaks, they are just not sharper). Past 85 % of the budget the replacements that nobody has drawn for 10 seconds
  (`hd_stale_frames`, 600 frames) are released, oldest first, down to 70 %: after a retry or the next mission the textures of the level you left make room for the new ones
  (without this the budget filled up after three retries and later textures stayed blurry; found in the Vita3K run). Every 30 seconds `log.txt` has a line `[gl] textures: ... | hd 2x: resident N tex ... KiB (peak ...) over budget N` with the numbers; the budget in use is
  printed as `[hdtex] mode 2x, budget 32 MB, pack of N textures ...` (again each time the mode changes).
* **Memory advice:** use the 2x pack unless the 4x pack is clearly smoother and sharper on your console; the 4x pack plus MSAA 4x plus a high enemy density is the combination most likely to run out. If the game
  crashes, runs slowly or shows white textures, lower the HD mode first (menu or `settings.txt`: `gfx.hd_textures=off`), then MSAA, then the pixel lighting.

### Textures whose colour table animates (the radar, fires, street lights)

The game changes the colour table of some textures every few frames (the radar picture in the corner of the screen about every other frame). A replacement for such a texture has to be
recoloured whenever the table changes, which costs a few milliseconds each time (4-6 ms with the 2x pack and 19-43 ms with a full 4x pack, on a desktop core). The port therefore watches every colour table:
the first few changes of a texture are followed, and if it keeps changing every few frames the **original texture is used while it keeps changing** and the replacement comes back after 10 seconds without a change.
In a mission-1-like scene with a changing radar this cut the recolouring work from 191 rebuilds (1,640 ms over 300 frames) without the watch to 6 rebuilds (26 ms) with it, and the radar still draws normally
(the radar has no sharper version while it animates). Lines `[hdtex] the palette of the texture at ... changes every few frames: its original is used while it does` appear once per texture in `log.txt`.

## Switches without a menu entry

Set in `config.txt` (`gl_ui_upscale=0`) or the environment (`INFECTED_GL_UI_UPSCALE=0`); both are on by default and both exist to be compared.

| Key | Default | What it does |
| --- | --- | --- |
| `gl_ui_upscale` | 1 | Small pixel-art textures that only 2D draws sample (button glyphs, HUD icons) are uploaded at twice their size, made by MMPX (`PortableKit/host/gpu/mmpx.hpp`). The log gets the count and the time per texture. |
| `gl_ui_upscale_budget_us` / `gl_ui_upscale_max_us` | 1500 / 3000 | What one frame may spend on these passes after the first, and the predicted cost over which a texture is left alone. 0 = no limit. The cost on the Vita has not been measured yet. |
| `gl_prewarm` | 1 | Links the shader programs earlier runs used while the renderer is idle. `shader_variants.txt` (this folder) is written by the game as programs link; `shader_cache/variants.txt` can ship in the package. |
| `gl_compact_vertices` | 1 on the Vita (0 in PortableKit itself) | Transformed (3D) draws are written to the GPU batch without the vertex fields their program does not read: 36 bytes a vertex when lit (no w), 24 when unlit (no normal), in place of 40. In a mission that is 15 % fewer vertex bytes a frame (890 KB to 759 KB on the PC replay); 2D draws keep the whole vertex. Pictures are identical (100 mission frames compared byte for byte, per-vertex and per-pixel lighting, single and split renderer). On by default on the Vita since 17 Oct 2026 on the strength of that replay and the byte count alone; it has not run on the console, and the speed is unmeasured there (the idea is that the render thread writes, and the GPU fetches, fewer bytes; the cost on the CPU is about the same, ARM instruction count +1.6 %). Set `gl_compact_vertices=0` to compare. `ge_replay --stats` prints the vertices written this way and the bytes saved. |
| `gl_rt_scenes` / `gl_rt_scenes_fbo` | 4 / same as `gl_rt_scenes` | The number of scenes per frame each render target is created for (`vglSetupRenderTargetScenesNum`, vitaGL's default is 1 and 1). In a traced mission frame three of the four guest render targets are begun twice. 0 = vitaGL's default; the value is clamped to 8. The hope is that the 8-20 ms first draw after a render-target switch shrinks; what going over the declared number costs on the console, and what 4 costs in memory, are both unknown. Read it with `gl_timing=60`: the `[gl-time]` line's first draw after a switch, against a run with `gl_rt_scenes=0`. The log line `GL: render targets are created for N scene(s) a frame` says what was asked for. |
| `INFECTED_NO_FUSED_DECODE` | off | Texture decoding goes through the general path instead of the fused palette path (identical pixels, slower). |

## Where the options come from (for developers)

| Layer | Where | Wins over |
| --- | --- | --- |
| developer override | `config.txt` (`gfx.texture_filter=original`) or the environment (`INFECTED_GFX_TEXTURE_FILTER`) | everything |
| the player's choice | `settings.txt`, written by the menu | the default |
| default | the table above | |

Older `config.txt` keys still work and override the menu: `threaded_renderer=0/1`, `gl_msaa=0/2/4`, `gl_vsync=0/1`, `hd_budget_mb=N`.

## What was and was not checked

Checked in the Vita3K emulator (software Vulkan, the emulator's own vitaGL), with the VPK from this build:

* The menu opened with the pad chord (SELECT + START, keys sent by a script) at the mission-1 briefing and during play, three times in one
  run; every row of both pages changed value; the menu closed and the game carried on from the same emulated moment (the log says
  `guest resumed ... emulated time N us (it was N us when the menu opened)`); picture and menu upright and readable; the FPS counter visible.
* HD textures: "Best available" (the 4x pack), Off, 2x pack and 4x pack switched while playing, and the very same recorded frame drawn
  with Off / 2x / 4x (pictures differ, nothing corrupt: DXT1, DXT5 and RGBA8 records with their mip levels all upload; "failed 0").
  One mission-1 street scene needs 172 textures: 4.9 MB of originals; with the 2x pack 7.2 MB; with the 4x pack 26.9 MB. Playing the
  first mission with the 4x pack the replacements on the GPU peaked at 61.4 MB of the 64 MB budget (about 245 textures at the start of the
  mission), and nothing was refused: with the 4x pack the budget will run out in busier places and those textures keep their originals.
  The 2x pack needs about a quarter of the memory.
* `perf.cpu_clock=500` asks the system for 500 MHz and logs that the clock stays at 444 (the emulator refuses it, as a console without
  the plugin does); `gfx.antialiasing=msaa4` makes vitaGL start with 4x multisampling; `perf.frame_cap=30` sets the display sync.
* No guest memory faults (`[Memory` errors) in any of the runs, and a clean exit with START + SELECT + L + R.
* The compiled shaders shipped in the VPK (35 programs, 70 files) are copied to a fresh data folder at the first start
  (`shader cache: 70 shipped file(s) copied`).

Not checked:

* **The touch-corner tap** (the emulator harness cannot touch).
* **Anything on a real Vita**: speed, GPU memory (whether 64 MB of HD textures plus the game's own fit next to the frame buffers), what the
  "cost" column really is, the 500 MHz clock, MSAA on the real GPU, tearing with the frame limit Off. The cost column is an estimate.
* The audio pausing by ear while the menu is open (the pause itself is logged; the desktop build checks the same code path).
* HD movies: see `KNOWN-ISSUES.md`.
* The first lines of `log.txt` list the settings before the renderer has looked for the packs, so the HD row can read
  `[greyed: No texture pack installed.]` there even with a pack in place. The line `[hdtex] mode ... pack of 2625 textures` a moment later
  is the one that counts, and so is the menu.
