# Infected for the PS Vita (release candidate)

This is the PSP game *Infected* running **natively** on a PS Vita. It is not an emulator. The game's code was translated to run
on the Vita's processor, and the PSP's graphics chip is imitated with the Vita's own graphics chip.
**The game itself is not in this folder.** You need your own copy (an ISO file).

**Short version: `INSTALL.md`.** Three steps: install the VPK, put your ISO in `ux0:data/infected/`, open the bubble.
This page is the longer version. `HARDWARE-TEST.md` is a checklist for the speed tests.

## What you need

* A Vita (or PS TV) with custom firmware (HENkaku or ensō), and VitaShell.
* The file `libshacccg.suprx` in `ur0:data/`. This is Sony's shader compiler, taken from your own console.
  The game ships with the shaders it needs already compiled, so it should not need it. Install it anyway, to be safe.
* Your copy of Infected: **US version, ULUS-10054, version 1.02**, as an ISO (any file name ending in `.iso`). Other versions are refused with a message.
* About **900 MB free on `ux0:`**. The ISO is 590 MB. The files in this folder are 236 MB. (Sizes in this folder are in millions of bytes.)

## Install

1. Copy `infected.vpk` to the Vita (USB or FTP). In VitaShell, open the file and press **CROSS**. An **Infected** bubble appears on the home screen.
2. Copy your ISO into `ux0:data/infected/` (any file name ending in `.iso`; start the app once if the folder is not there).
3. Optional, sharper textures: copy **everything inside** the folder `ux0_data_infected` into `ux0:data/infected/` (the two texture packs,
   the `dumps` folder for the graphics speed test, `bench_profile_ms0` and `config.txt.example`). The packs are all you need for play.
4. Open the bubble. The first start shows a boot screen with a few lines, then the game's logos and movies. Press CROSS to skip a movie.

If you use RetroFlow, the game's cover and background appear there by themselves after the first start.

## Controls

The Vita's buttons are the PSP's buttons: CROSS, CIRCLE, SQUARE, TRIANGLE, the D-pad, L, R, START, SELECT.
The **left stick** is the PSP's analog stick. The game's own **CONTROL CONFIGURATION** screen (START, then choose it) shows what
each button does. In a mission: CROSS fires the pistol, SQUARE fires the viral gun, R locks on, TRIANGLE closes tutorial pop-ups.
The touch screen and the right stick are not used by the game. (The top-right corner of the touch screen opens the settings menu.)

## Leaving the game

Hold **START + SELECT + L + R** together for about one second. The game stops and writes its log.
If the game has frozen, the same four buttons held for three seconds end it as well.

## The settings menu

While you play, hold **SELECT + START** for half a second. You can also tap the **top-right corner of the touch screen**.
The game pauses and a menu opens. It has two pages, Graphics and Performance.

* **UP / DOWN**: choose a row. **LEFT / RIGHT**: change the value.
* **L / R**: switch page. **CIRCLE** or **START**: close the menu. Your choices are saved.

Every option, and what it costs, is in `SETTINGS.md`. A row shown in grey cannot be used on this build.
The menu always starts with safe values: sharper textures from the 2x pack, no anti-aliasing, 60 frames per second at most.

## What each file in this folder is

| File | What it is |
| --- | --- |
| `infected.vpk` (5.7 MB) | The game program. Install it with VitaShell. It contains no game data. |
| `ux0_data_infected/texpack_2x.bin` (72.2 MB) | Sharper textures, twice the original size. Used by the "HD textures" option. |
| `ux0_data_infected/texpack_4x.bin` (102.4 MB) | Sharper textures, four times the size for the world and effects, twice for characters and menus. |
| `ux0_data_infected/dumps/` (55.4 MB) | 100 recorded frames of mission 1, for the graphics speed test (`bench=ge_replay`). |
| `ux0_data_infected/bench_profile_ms0/` (0.3 MB) | A game profile (memory stick folder) for the bot speed tests. Not used by normal play. See `HARDWARE-TEST.md`. |
| `ux0_data_infected/config.txt.example` | Optional extra settings. Rename it to `config.txt` to use it. All lines in it are off. |
| `INSTALL.md` | The three-step install. |
| `README.md` | This page. |
| `SETTINGS.md` | The settings menu, option by option. |
| `HARDWARE-TEST.md` | The checklist for your first run, and which files to send back. |
| `CHANGES.md` | What is new in this build. |
| `KNOWN-ISSUES.md` | What is not checked yet, and what is known not to work. |
| `../infected.elf` (108 MB, next to this folder) | For the developer only. It turns crash addresses into code lines. Do not copy it to the Vita. |

## Files the game makes on the Vita (in `ux0:data/infected/`)

| File | What it is |
| --- | --- |
| `log.txt` | The log of the **last** run. It is rewritten every time you start the game. **Send this back.** |
| `settings.txt` | Your menu choices. Delete it to go back to the defaults. |
| `ms0/` | The game's save data. The PSP memory stick is a folder here. |
| `shader_cache/` | Compiled shaders. The game fills it. Leave it alone. |

## HD movies (not included, experimental)

The game has a set of 18 movies remade at 960x544. They are **not in this folder** and are not needed.
They are delivered separately: 18 files plus `index.txt`, about 394 MB, from the folder `movies_hd` on the build machine
(`/work/ws/hdmov/private/hdbuild/movies_hd/`). On the Vita they would go to `ux0:data/infected/movies_hd/`.
The option stays grey until that folder is there. On a real Vita this feature has never run. See `KNOWN-ISSUES.md`.
