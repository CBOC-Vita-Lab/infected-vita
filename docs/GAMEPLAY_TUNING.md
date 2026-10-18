# Gameplay tuning: enemy density and view distance

Two switchable enhancements of the native Infected port, both implemented as data pokes made from `GameProfile::frame_hook`
(`port/host/infected_tuning.cpp`), with no change to the recompiler, the lean corpus or the interpreter:

| setting | values | what it does | when it takes effect |
|---|---|---|---|
| `perf.view_distance` | 100 / 150 / 200 (%) | scales the far plane and the fog start of every cell's environment record | live, within about half a second |
| `perf.enemy_density` | 100 / 125 / 150 / 200 (%) | scales the number of enemies a cell keeps alive and how fast it refills them; pools sized for it | the number is read when a mission loads (latched per level: the mission in progress keeps its number); the pools are rebuilt once, at start-up, in the front-end or in the gap of a retry (every pool unused), see Limits |

Both are read through ONE function, `tuning_setting(key, fallback)` (`infected_tuning.cpp`), which in the merged tree reads the player's settings through
`host/settings/port_settings.hpp` (the in-game menu rows `perf.enemy_density` and `perf.view_distance`; `config.txt` / `INFECTED_PERF_ENEMY_DENSITY` / `INFECTED_PERF_VIEW_DISTANCE`
override them like every other option). `tuning_install` enables the two rows (`Settings::set_unavailable(key, nullptr)`; a program without this module keeps them greyed with "Only while the game runs.").
Values are clamped to 100..200. 100 means "do nothing": no memory is written, the pools are not rebuilt, the run is bit-identical to a build without the module
(checked: the 100 % bench window has the same entry and draw counts as before the module existed).
**Enemy density is latched per level** (merged tree): each cell learns its own numbers when the level is built and keeps the percentage that was set at that moment (`view::State::level_serial`),
so a change in the menu in the middle of a mission applies at the next mission load (start, retry, next mission) and is logged as `[tune] flip N: enemy density setting A% -> B% (applies at the next mission, ...)`;
the overlay row says "next mission". View distance is live.

Addresses below are for the relocated US 1.02 image (ULUS10054, `BOOT.BIN` = PRX `Rw37Skel`, RenderWare 3.7, image base `0x08804000`;
file offset in `work/image.bin` = address - `0x08804000`). Another release needs these found again: every table and class below is
identified by strings that exist in the image (`CCell`, `CellVolume`, `CNpcGen`, `NpcAllocator`, `NpcGhostAllocator`, `CWorldRender`,
`ACTN_CELLCLEANED`, `ACTN_OVERLOADEMINENT`), see "How it was found".

## 1. View distance

### What bounds it
* The world is drawn by RenderWare through ONE camera (pointer `*(0x09ED32C0)` while the world is rendered). Its planes are RenderWare's
  `RwCamera` fields: near `+0x80`, far `+0x84`, fog start `+0x88`, zScale `+0x8C`, zShift `+0x90`. The GE gets the fog from the game's
  libgu `sceGuFog(near=fog_start, far=far, colour)` at `0x08A26E4C` (only caller: the state flush at `0x0899C85C`), so `far` is both
  the clip distance and the end of the fog ramp.
* The numbers come from the level data. `CWorldRender` (singleton pointer `0x08AD0450`, 0 between levels; creator `0x08A4D4BC`, ctor `0x0886DAD0`,
  0x1C0 bytes) owns three environment records of 24 bytes `{name, near, far, fog_start, colour, fade_seconds}` at `+0x68` (from), `+0x80` (to), `+0x98`
  (current, blended over `fade_seconds`). Every frame the message handler `0x0886EC30` -> `0x0886EB98` -> `0x0886E020` copies the CURRENT record into the
  camera with `RwCameraSetNear/Far` (`0x0898C868` / `0x0898C8A8`, which recompute zScale/zShift) and sets the fog start.
* Records are filled from the `CellVolume` entity of the cell the player enters (class registered `0x088C0910`, creator `0x08A55360`, ctor `0x088BFAB4`,
  0x140 bytes, vtables `0x08AD25F0` at `+4` and `0x08AD2610` at `+0x1C`); its record is at `+0xEC` (near `+0xF0`, far `+0xF4`, fog `+0xF8`, colour `+0xFC`,
  fade `+0x100`; constructor defaults 1 / 1100 / 550) and is copied by `0x0886F5DC` on entering. Level attributes 6..10 of `CellVolume` are
  near / far / fog / colour / fade. 354 CellVolumes in 49 level streams: far from 500 to 2000 (1000/500 in 88, 1100/550 in 71, 800/50 in 47, 2000/50 in 43...).
* Nothing else limits it in the code: the content does (BSP sectors outside the frustum and beyond `far` are not drawn).

### What the setting does
`view::frame` finds every CellVolume (both vtable words and a sane record), keeps the original far and fog start, writes `original * k` into the CellVolume records,
and fixes up the three live records of CWorldRender when they hold unscaled values (a change of the setting at run time scales them by the ratio). The game then
copies, blends (fade) and applies the scaled numbers by itself, so fade between cells, fog colour and sky stay consistent. Near is not touched.

### Evidence
* Hook proof: `INFECTED_TUNE_VIEW_RAW=0.25` (far 450, fog 1.25) clips the end of the street and shows the sky dome behind it
  (`private/shots/raw_cmp_4100.png`); 0.12 clips at 216 units.
* Mission 1 (`Mission_1A`): one cell BSP (`midtown_cell_10.bsp`, 2597 triangles, bounding box 1270 x 504 x 183, a 1270-unit street) and `far 1800`, `fog 5`.
  The gameplay draws reach view depth 350..550 (`ge_replay --analyze --zstats` on the GE dump of flips 4100..4102). Far 1800 is already beyond
  the end of the content, so at 100 % the horizon is bounded by the CONTENT (burning wrecks, smoke, buildings) and 200 % only thins the haze (fog 5..1800 becomes 10..3600):
  mean absolute pixel difference 8/765 at the top of the picture, 0.8 at the bottom (`private/shots/v_diff_4100.png`). This is the honest limit of the feature:
  it helps in levels whose far plane is shorter than the world, not in mission 1.
* Levels where it matters (CellVolume far < extent of the world, `tools/bspext.py` + `private/logs/cellvols.txt`): `Mission_9A` (far 1100, cell BSPs up to 2723 wide),
  `Mission_4A` (500 / 1200 against 2284), `Mission_14B` (1200 / 2284), `Mission_10A` and `Mission_12A` (2000 / 2723), `Mission_5A`, `Mission_6A`, `Mission_2B`, `Mission_14A` (800..1800 / 2612).
* Depth precision: RenderWare's depth is `zScale / z + zShift` with near = 1; for far >> near the depth step at distance z is about `z^2 / (near * 65535)` and does
  not depend on far (factor `far / (far - near)`, 1.0003 for 1800 -> 3600), so a longer far plane does not add z-fighting; it only lets distant surfaces (that were clipped) be
  drawn with the same coarse step they would have at the same distance.

(Numbers of the measured costs and the sheets are in section 3.)

## 2. Enemy density

### What bounds the number of enemies
* Every level has one `CCell` object per cell (class creator `0x08A552B4`, ctor `0x088BCB0C`, 0x240 bytes; class name string `CCell`). The cell manager singleton pointer is
  `0x08ACDD18` and keeps the `CCell*` vector at `+0x28` .. `+0x2C`. A cell owns its generators (`CNpcGen`, array of pointers at `+0x3C`, count at `+0x64`, remaining
  supply at generator `+0x11C`) and two lists of the NPCs that belong to it at `+0x170` (index 1 and 2; their sizes are what `0x088BCCD0` / `0x088BCCF0` return).
* Fields (all loaded from the level's `CCell` attributes): `+0x94` cell id; **`+0x98` the maximum number of NPCs alive in the cell** (mission 1: 4; the 36 missions use 1..15);
  `+0xA0 / +0xA4` minimum / maximum of the second list (0..30 usually, 1..2 .. 1..5 in some cells); `+0xB0` ratio 0.51; `+0xB4 / +0xB8` initial delay (15 s);
  `+0xC4 / +0xC8` overload countdown (360 s: `ACTN_OVERLOADEMINENT` at 60 s left, `ACTN_OVERLOADACTIVATED` at 0); **`+0xDC / +0xE0` spawn spacing** (1..1 s in mission 1, 5..10 s later);
  `+0x17C..+0x18C` timers, `+0x194` round-robin index of the generators, `+0x230` state (0..6; 3 = list 2 is at the cap, 4 = empty, 6 = cleaned).
* The cell's tick `0x088BD9DC` (called through the dispatcher `0x0888839C`) spawns only when the state allows it, the timers `+0x184` and `+0x18C` are negative,
  `alive = list1 + list2 < cell+0x98` (`0x088BD040`), and `list2` is within `[+0xA0, +0xA4]`. It then calls `0x088BD074`, which walks the generators round robin
  (`+0x194`), skips those with nothing left (`+0x11C == 0`), calls `CNpcGen::Spawn` (`0x08819C34` -> `TrySpawn 0x0881D9FC` -> `CreateGhost 0x0881D964` when the cell is not
  the active one, `CreateInstance 0x0881D89C` when it is) and sets `+0x184 = lerp(+0xDC, +0xE0, random)`. Cap getter `0x088BD1C4` is also used by the "pick a cell with room" search
  `0x088BD1CC` and by the sum of capacities `0x0886B850` (HUD).
* Mission 1 therefore: 4 `CNpcGen` (remaining 10, 10, 10, 7 = 37 enemies in total), cap 4, spacing 1 s. The cap counts EVERY NPC of the cell, also the ones the level places by hand (mission 1 has 2 of them
  at the start: `alive 0+2`, the generators add 2 more up to the cap), so the NPC pool `used` is 4 for thousands of flips until the player kills something, and each kill is replaced about a second later
  until the 37 are spent. A cap is not a hard limit: `Mission_9A` has a cell with cap 9 and 14 NPCs in it (placed ones, scripted additions); only the spawner obeys it.
* Behind the cap are fixed-size pools (allocator pointer table `0x09EAC5E4 + 4 * id`; pool object: `+0x64` element, `+0x68` count, `+0x6C` used, `+0x70` peak, `+0x74` built flag,
  `+0x78` storage, `+0x7C` free list; `alloc 0x08897270` returns 0 when `used == count`; built once at start-up by `0x08892BF0` through `pool_init 0x0889700C`; `pool_term 0x0889718C`):

| id | pool | element | count | grows with enemies by |
|---|---|---|---|---|
| 1 | Npc | 0x540 | 20 | one per live NPC (also scripted placed `CNpc`) |
| 2 | NpcGhost | 0xD0 | 105 | one per NPC of a cell the player is not in |
| 3 | Pickup | 0x1C0 | 50 | drops |
| 4 | Projectile | 0x1AC | 50 | enemy shots |
| 5 | Weapon | 0x5AC | 80 | NPC weapons |
| 6 | FxSimple | 0x150 | 128 | particles (mission 1 peaks at 47 with 4 enemies) |
| 7 | ACollidable | 0x18 | 200 | |
| 8 | VoiceHandle | 0x8 | 50 | |
| 17 | GraphicalAsset | 0x24 | 400 | (91 used, peak 121 in mission 1) |
| 24 | AGraphical | 0x10 | 200 | |
  (`id 0` Stl 15360 x 0x20 and `id 32` Null are not per enemy.) NPC creators (alloc 0x540 from id 1): `0x0881D89C`, `0x08A415E0` (+ctor `0x0881E8B0`), `0x0881E414`, `0x0883A394`, `0x08A41D08`.
  Kaim AI limit: `MaxEntity 121` (config `0x0887818C`), far above the 40 NPCs possible with the enlarged pool.

### What the setting does (`density::frame`, every 4 flips)
1. **Pools.** The first time the setting is above 100 while every pool is unused (the first flip of a start-up with the setting raised, or the front-end after it was raised in a menu: the pools
   are free until a mission is loaded), every pool of the table above is rebuilt twice as large (`pool_term` + `pool_init` as nested guest calls; refuses and asks again 4 flips later when anything is
   allocated or the game's own count differs from the expected one; restores the original size if the heap has no room). +287 KB in total. At 100 % nothing is touched.
2. **Cap.** For each cell: `cap = max(base, min(round(base * pct / 100), alive + room))` where `room` is the smaller free room of the Npc and NpcGhost pools minus a reserve of 4
   slots (scripted NPCs and a burst of spawns), or 0 when less than 1 MB of the game's heap is free (dlmalloc top + victim chunk at `0x08ADD25C`, +0xC / +8; every extra NPC costs about 75 KB of heap
   for its clump, animation and weapon data), so the cap never exceeds what the pools and the heap can hold and never drops below the level's own cap. The cell's maximum for the second list (`+0xA4`) is scaled alike.
3. **Rate.** The spawn spacing `+0xDC / +0xE0` is divided by `pct / 100`, so a cell refills at the same relative speed.
4. **Not touched on purpose:** the supply (`CNpcGen +0x11C`: 37 enemies in mission 1), the overload countdown, the medal thresholds and every objective: a level asks for the same number of kills in the same
   time, only more of them are on the street at once. (Scaling the supply too was rejected: it would make the 360 s overload countdown and the medal times unreachable at 200 %.)
   Levels whose supply is smaller than the cap (`Mission_4A` 12 enemies for caps 10 + 10, `Mission_9C` 5) show no difference: they are supply-bound.
5. A cell that the game writes again (a retry builds a new level) is learned again from the live value, so the scaling never compounds.

Caps of the 36 missions: 1 (one cell of `Mission_14C`) .. 15; with 200 % the 15-caps become 30 against a pool of 40 (the same 0.75 ratio the game has at 100 %: 15 of 20).

## 3. Verification

All runs are deterministic (lean code only, virtual clock, flip-keyed pad script `tools/tune/stab.script`: the menus, mission 1 from flip 5493, the player walks and fires in a loop), so a counter that differs between
two runs differs because of the setting. "God mode" (`INFECTED_TUNE_GOD=1`, a probe that keeps the player's health at its maximum) is used where the run has to last. The scripted player is weak: in 30000 flips it kills 6 of mission 1's 37 enemies at 100 % and at 150 %
and 21 at 200 % (more targets in reach), it cannot finish a mission.

### 3.1 View distance
* **Same-state pictures** (`private/showcase/view_distance_*.png`, real GL renderer, 960 x 544, the game state is identical in the three columns because the setting only changes the camera planes and the fog):
  `Mission_9A` (all 21 of its CellVolumes: near 1, far 1100, fog start 550) at flip 6000 shows the skyline of Central Park: at 100 % the buildings, beyond the fog start, are inside the fog ramp 550..1100 and look pale; at 200 % the ramp is 1100..2200 and they are dark with their window lights.
  The difference picture is zero everywhere except the skyline (HUD regions: score / time, top bar, weapon boxes 0.00 mean absolute difference; the radar region differs only through the skyline seen behind its translucent ring).
  Mission 1 (`view_distance_mission1.png`) differs by haze over the whole street (mean 1.6 / 255 at flip 6000, 0.4 at 7200). The HUD itself is not touched: the GE statistics of the 600-flip windows (through-mode draws 25501, fogged, blended, textured
  counts ...) are identical at 100 / 150 / 200 % (`diff` of `ge_win.txt` is empty); where the HUD plates are translucent their pixels differ by up to 4 / 255 because the world behind them is fogged differently.
  Where nothing is beyond the old fog start the three columns are identical (`Mission_9A` flips 6600 and 7200: mean absolute difference 0.00 over the whole picture).
  No hole, no flicker, no z-fighting appears at 150 / 200 % (the same pictures, nothing new in the difference image).
* **Where the fog is dense the difference is large**: `Mission_4A` (CellVolume far 500 / 1200 with a short fog ramp) seen through the mission-1 slot, `view_distance_mission4A.png`, flip 5900: at 100 % the whole park is in grey haze and the buildings
  are washed out, at 200 % the park and the skyline are clear (mean absolute difference 15.6 / 255 over the picture). `view_distance_mission14B.png` (far 1200 / 1100, fog start 1 / 10): the skyline is clearer at 200 % (1.4 / 255). The HUD plates of the
  `4A` flip 5900 pair differ in their fade-in phase (the level streams from the disc, the two runs reach the mission a few flips apart); the second pair (flip 6100, a tutorial dialog) differs only in the world behind it.
* **Why there is no z-fighting:** the depth value is `zScale / z + zShift` with near 1, the 16-bit step at distance z is about `z^2 / (near * 65535)`, which does not depend on far
  (the far term is `far / (far - near)` = 1.0003 at 1800, 1.0001 at 3600). A longer far plane only lets distant surfaces, which were clipped, be drawn with the precision they would have at that distance anyway.
* **The cost is on the GPU, not on the CPU.** The engine submits the same geometry whatever the far plane is (it does not cull by it): mission 1, flips 8000..8600, far 450 / 900 / 1800 (`INFECTED_TUNE_VIEW_RAW` 0.25 / 0.5 / 1.0)
  gives 463.28 draws, 8989.55 vertices and 4171.0 lean entries per flip every time; `Mission_9A` far 275 / 550 / 1100: 339.08 / 339.08 / 340.1 draws, 11806 / 11806 / 11811 vertices. With the real setting (100 / 150 / 200 %) the counters are
  identical in mission 1 and `Mission_9A`; over nine levels (`tools/tune/sweep.sh`, flips 6000..6120 of each level) draws change by 0 (`10A`, `12A`, `5A`, `2B`, `14A`), +0.15 % (`6A`), +0.9 % (`14B`) and +1.5 % (`4A`).
  Triangles beyond the old far plane that the GPU used to clip are now rasterised (and fogged less): that is the real price, a fill / clipping cost that only exists where there is content out there. It was NOT measured: the host renderer is a software rasteriser on a machine shared with other jobs, so its time says nothing about a Vita GPU.

### 3.2 Enemy density
Runs (`tools/tune/sustain.sh`: stab flow, 30000 flips = 8.3 minutes of game for mission 1, 20000 flips = 5.6 minutes for `Mission_9A`, probes every 600 flips):

| run | setting | cap (per cell) | NPCs alive (min..max) | Npc pool peak / size | NpcGhost peak / size | free heap (dlmalloc top) at flip 6000 / 12000+ | result |
|---|---|---|---|---|---|---|---|
| mission 1, god | 100 | 4 | 4..4 | 5 / 20 | 2 / 105 | 6.38 / 6.08 MB | 30000 flips, no stall |
| mission 1, god | 150 | 6 | 6..6 | 7 / 40 | 2 / 210 | 6.04 / 5.69 MB | 30000 flips |
| mission 1, god | 200 | 8 | 8..8 | 9 / 40 | 2 / 210 | 5.97 / 5.58 MB | 30000 flips |
| mission 1, no god | 150 | 6 | 6..6 | 7 / 40 | 2 / 210 | 6.04 / 5.69 MB | the player dies at flip 12028, the level is rebuilt, runs on to 30000 |
| mission 1, no god | 200 | 8 | 4..8 | 9 / 40 | 2 / 210 | 5.97 / 5.58 MB | dies at 14548, rebuilt (heap back to 5.93 MB), runs on to 30000 |
| mission 1, no god | 100 | 4 | 0..4 | 5 / 20 | 2 / 105 | | dies at 13108, rebuilt, runs on (19800 flips) |
| `Mission_9A`, god | 100 | 6 / 8 / 9 / 7 / 8 | 1..14 | 10 / 20 | 38 / 105 | 3.47 / 3.04 MB | 20000 flips |
| `Mission_9A`, god | 150 | 9 / 12 / 14 / 11 / 12 | 1..20 | 14 / 40 | 49 / 210 | 3.09 / 2.60 MB | 20000 flips |
| `Mission_9A`, god | 200 | 12 / 16 / 18 / 14 / 16 | 0..21 | 18 / 40 | 54 / 210 | 2.92 / 2.45 MB | 20000 flips |

Reading the table: the caps follow `round(base * pct / 100)`; the NPCs alive reach the caps (mission 1: exactly cap, from the first second); no pool comes near its (doubled) size; the free heap is 0.5 .. 0.6 MB lower at 200 %
(about +287 KB of pools plus about 75 KB per extra NPC) and does not drift (no leak across a level rebuild); `Mission_9A` is the tight level (2.45 MB free at 200 %), which is why the cap stops growing below 1 MB free.
`Mission_9A` cell `id3` (5 placed NPCs, no generator) was emptied by the god-mode player at 200 % (state 4 reached, the game's own "empty" transition ran) and never at 100 %: the scripted player simply reaches more targets.
* **Enemy counts from the game's own data** (cell lists, `INFECTED_TUNE_CELLS`): mission 1 has 4 / 6 / 8 NPCs alive at flips 6000, 6600 and 7200 at 100 / 150 / 200 %; `Mission_9A` at flip 8400 has 36 / 45 / 53 alive over its five cells
  (the 12 NPCs the level places in cells `id3` and `id7` do not change; the other three cells hold 24 / 33 / 41).
* **Fail and retry** work at every level (the three no-god rows): the player dies, the world renderer is destroyed and rebuilt (`0x08AD0450` 0 -> new), the cells are learned again (cap 8 again, not 16), the pools stay doubled.
* **Changing the setting in the front-end** (`private/data_rt1`, `settings.txt` rewritten from 100 to 200 at flip 1225): the pools were rebuilt at flip 5409, when the last front-end allocation was gone, and mission 1 ran with cap 8 and pools 40 / 210.
* **Pictures:** `private/showcase/enemy_density_mission1.png` and `enemy_density_mission9a.png` (same flips at 100 / 150 / 200 %). In `Mission_9A` (real GL renderer, god mode, flips 6000 / 6600 / 7200 / 8000) the radar of the 150 and 200 % columns carries visibly more
  red blips, more infected stand in the park (flip 6000: three at 100 %, a crowd at 200 %), and at 200 % the player is in melee range at flips 6600 and 8000 (the score is 1750 against 500: the scripted player reaches more targets). The pictures are not the same game state
  (the enemies change what the player does), unlike the view-distance sheets.

### 3.3 Costs (flips 8000..8600 of the stab flow, god mode, `tools/tune/bench_one.sh`, GE statistics of the window; deterministic counters, wall time on this shared machine is too noisy to quote)

| level | setting | NPCs alive | lean function entries / flip | draws / flip | vertices decoded / flip | guest code ms / flip (best of the runs) |
|---|---|---|---|---|---|---|
| mission 1 | 100 | 4 | 4171 | 463 | 8990 | 0.9 |
| mission 1 | 150 | 6 | 4748 (+14 %) | 651 (+41 %) | 12584 (+40 %) | 1.3 |
| mission 1 | 200 | 8 | 5195 (+25 %) | 662 (+43 %) | 13137 (+46 %) | 2.5 |
| `Mission_9A` | 100 | 5 cells, 38 caps | 8234 | 340 | 11811 | 0.9 |
| `Mission_9A` | 150 | | 9395 (+14 %) | 471 (+39 %) | 12741 (+8 %) | 1.8 |
| `Mission_9A` | 200 | | 11516 (+40 %) | 718 (+111 %) | 25762 (+118 %) | 2.0 |

An NPC costs about 250 lean entries and 30..50 draws per flip (skinned body, weapon, particles). The guest milliseconds were measured on a machine with a load average of 8..10 (best of 2..3 runs of the same state in mission 1,
a single run in `Mission_9A`): use their ratio (mission 1: 1 : 1.4 : 2.8, a lean entry of an NPC's animation / skinning code is heavier than the average one), not the values. The windows contain kills and respawns, so a single window is an indication, not a constant.
Determinism check: `bo200x100` run twice gives identical counters to the last digit.

## 4. Limits, clamps and what is not done
* **100 % is exactly the original** (no write at all); values are clamped to 100..200 (125 is accepted, the game-side tables only matter for the caps).
* **The enemy total is not scaled.** The supply of every generator (`CNpcGen +0x11C`), the overload countdown, the medal thresholds and every objective are untouched: the player kills the same 37 enemies in mission 1, only more are on the street at once.
  So "KILL 'EM ALL" and the civilian objectives are the same; scaling the supply would make the 360 s overload timer unreachable.
* **Cap above what fits is refused**: the cap is `min(scaled, alive + room)` with room from the Npc / NpcGhost pools (reserve 4) and 0 when less than 1 MB of heap is free. A cell cannot be pushed past its own level cap by the pools.
* **Pools are rebuilt once** (x2, when everything is unused: at start-up or in the front-end). A setting raised in the middle of a mission keeps the old pools: the cap then grows only as far as the original pools have room
  (mission 1: 20 NPCs, `Mission_9A`: the 20 / 105 pools reach 10 / 38 NPCs at 100 %, so little room), and is applied fully from the next time the front-end is visited. Lowering the setting at any time restores the level's own caps live; NPCs above the cap stay until killed.
* **Not covered:** the pools that grow with other things (`Stl` 15360 nodes: 7672 used at the title, 7900..9400 in missions; `Weapon`, `Pickup`) are doubled only where listed in section 2; the Kaim AI entity limit `MaxEntity 121` is far above 40 NPCs.
* **Mission completion at 150 / 200 % was NOT verified by playing a mission to its end.** The scripted player cannot do it (it camps at the start, 6 .. 21 of 37 kills), and the developer kill harness (`INFECTED_TUNE_KILL`, a lethal hit through the game's own
  hit path `0x088A54F0` -> `Npc::TakeDamage 0x0881F6FC`, damage = the victim's health) kills NPCs whose corpses are removed normally for the first ~14 kills (60..236 flips after the blow) and then leaves corpses that are never removed (hit points 0,
  flags `+0x59 = 0x15`, no death animation started), at every density. The same happens when the blows are struck from inside the game thread (`INFECTED_TUNE_KILL_HOOK=1` wraps the cell tick `0x088BD9DC`), so it is not a threading artefact;
  the NPC words that were compared (type, animation pointers) do not separate the removed from the stuck ones. The natural kills of the scripted player are always removed (at 200 % the 8 NPCs alive at flip 20000 are healthy and idle at their spawn point).
  So the harness is not a valid completion test. What was verified instead: fail / retry, the cell state machine (`0x088BCF0C`: state 0 / 1 / 2 / 3 / 4 from the two list sizes and the cap, unchanged by the setting) reaching 4 at 200 %
  (`Mission_9A` cell `id3`), and that nothing the objectives read (supply, medal data) is written. **Next experiment:** a pad script that really kills every enemy (they idle at the spawn points near x 1260, z 420 and attack when the player
  comes within sight: walk to them and fire), or find what the stuck corpses wait for (the death animation of those NPCs never starts: a write watch on the NPC's `+0x58` does not see lean stores, so use the interpreter on `0x0881FA94` /
  `0x08821F28`, the code that sets the dead flag).
* **View distance helps only where the content is beyond the old far plane or fogged by it** (`Mission_9A`, `14B`, `4A`, the 800..2000 far levels with fog start 50..550); mission 1 (far 1800, a 1270-unit street) only loses haze. Near is never changed.

## 5. Risks
* Memory: `Mission_9A` at 200 % leaves 2.45 MB of the 24 MB heap free; the guard stops growth at 1 MB. A level with more resident data than `Mission_9A` could hit the floor at lower density and then stay at its own caps (safe, not 200 %).
* Frame time: +40 % draws (150 %) and +43..111 % (200 %) of skinned geometry per frame on the Vita GPU; a frame-time budget is not measured here (host numbers only).
* The kill harness / probes are developer tools, off by default, and must not be exposed in a settings screen.
* The cap writes happen from the frame hook every 4 flips on the same thread as the guest flip; the cell tick may run between a flip and the next write with the previous cap (harmless: the cap changes by whole NPCs).
* The settings function reads `settings.txt` by mtime and size (one second granularity); the lead replaces it by `host/settings/port_settings.hpp`.

## 6. How it was found and how to reproduce
* Tools (`tools/tune/`): `reloc_dis.py` (image.dis -> one line per instruction with guest addresses), `d.sh ADDR [N]`, `xrefidx.py` + `funcs.py` (xrefs, function starts, callers),
  `rwsent.py` (RenderWare Studio attribute records of an inflated level stream), `pools.py` (the pool table), `bspext.py` (BSP bounding boxes), `mkpad.py` (pad scripts), `play.sh`, `bench.sh`, `bench_one.sh`, `raw_bench.sh`,
  `density_bench.sh`, `sustain.sh`, `summ.py` (summary of a sustain run), `shots.sh` (GL screenshots), `sheet.py` (comparison sheets with difference column), `sweep.sh` (levels through an ISO swap).
* Developer probes in `infected_tuning.cpp` (all off by default): `INFECTED_TUNE_LOG=1`, `INFECTED_TUNE_CAMLOG=N` (camera planes), `INFECTED_TUNE_POOLS=N`, `INFECTED_TUNE_CELLS=N`, `INFECTED_TUNE_GENS=N`,
  `INFECTED_TUNE_NPCS=flip:count`, `INFECTED_TUNE_DUMP=flip:addr:words`, `INFECTED_TUNE_VIEW_RAW=k` (unclamped view factor), `INFECTED_TUNE_GOD=1`, `INFECTED_TUNE_SUPPLY=k`, `INFECTED_TUNE_KILL=from:age[:per]`
  with `INFECTED_TUNE_KILL_DMG` and `INFECTED_TUNE_KILLWATCH`.
* Another level through the mission-1 entry (the disc is not modified): `iso_swap.py` of the texture work (`--redirect .../Mission_1A.stream.zpm .../Mission_9A.stream.zpm` on a COPY of the disc), then `DISC=<copy>` for `tools/tune/run.sh`.
* Reproduce (workspace `/home/claude/ws/tune`, build in `build/`, private data in `private/`; every command nice'd, one run at a time on this machine):
  * `tools/sustain.sh g200 200 INFECTED_TUNE_GOD=1` then `python3 tools/summ.py g200` (mission 1, 30000 flips; no god: omit the variable; `Mission_9A`: `DISC=$PWD/private/swap/iso9a.iso FRAMES=20000 tools/sustain.sh m200 200 INFECTED_TUNE_GOD=1`)
  * `tools/bench_one.sh bo200x100 200 100` (density 200, view 100; add `mission9a` as 4th argument for `Mission_9A`), `tools/raw_bench.sh rbo0.25 0.25` (far forced to a quarter)
  * `tools/shots.sh sd200 5800,6200,6600,7000 7100 INFECTED_PERF_ENEMY_DENSITY=200 INFECTED_TUNE_GOD=1`, `tools/sheet.py OUT.png TITLE "label=a.png,b.png,c.png" --cols 100%,150%,200% --diff`

## 7. In the merged tree: settings wired, Vita build (2026-10-10)
* `tuning_setting` reads `host/settings/port_settings.hpp`; both rows are usable; `infected_tuning.cpp` is built into the Vita app (`vita/app/CMakeLists.txt`). Cost hints in the overlay: enemy density {100 none, 125 medium, 150 high, 200 high},
  view distance {100 none, 150 medium, 200 medium} ("GPU cost not measured"). The measured costs of section 3.3 stand for the desktop counts only; nothing was measured on a GPU or a Vita.
* Desktop live check (lean code, threaded renderer, the Vita's default settings and packs; `scripts/g1_live.sh`, stab flow to mission 1, the menu opened by the pad chord at flip 6000, Performance page, both rows to 200 %):
  * view distance changes the picture at the next flip after the menu closes (`[tune] flip 6031 view distance 100% -> 200%`; pictures identical up to flip 6032, different from 6034 on: mean |diff| 1.6/255 over 82 % of the pixels, the fog at the end of the street);
  * enemy density: cap 4 / alive 4 stays for the 11,400 flips after the change (the mission keeps its number), the scripted player dies at flip 17308, the pools are enlarged in the gap (flip 17309, every pool unused),
    the new level has cap 8 and 8 NPCs alive (Npc 8/40), the supplies are untouched (gens 9 8 8 6). With 200 % from the start the first level has cap 8 and the retry again cap 8.
  * the pad script of a long run must not end before the retry briefing: the mission waits for a button.
* An attempt to force a retry with a lethal blow on the player from the flip hook (the NPC kill harness's path) set the player's health to 0 but the game never ran its failure path (no retry within 2,400 flips): not kept.
  Retries were reached by the scripted player dying by itself (desktop flip 17308; Vita3K, an idle player against 6 or 8 enemies, every ~700 flips) and by the game's pause menu (START, DOWN to RESTART, CROSS: shown on the desktop).
* Vita3K (the VPK of this build, 2x + mixed 4x packs installed, default settings): the menu opened in the front-end at flip 1213 set enemy density 150 %; `[tune] flip 1861: enemy density pools enlarged x2`;
  mission 1 loaded at flip 1943 with cap 6 and 6 NPCs alive (`gens 9 9 9 6`, supply untouched); the player died at 2458 and the retry (2543) had cap 6 again; at flip 3465 the menu set 200 % (`[tune] flip 3465: enemy density setting 150% -> 200%
  (applies at the next mission, the cells of this one keep their number)`; cap 6 and 6 alive until the next death) and view distance 200 % (`[tune] flip 3481 view distance 100% -> 200%`); the player died at 3858, the retry at 3943 had **cap 8, 8 NPCs alive** (Npc 8/40).
