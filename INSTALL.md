# Install Infected on a PS Vita

You need a Vita (or PS TV) with HENkaku or ensō and VitaShell, your own copy of the PSP game **Infected** (US version, ULUS-10054,
1.02) as an ISO file, and an `infected.vpk` that you built from that copy.

**There is no VPK to download.** The package contains code translated from the game, so it cannot be shared. Building one from your own
disc is a one-time job on a PC: [`docs/BUILDING.md`](docs/BUILDING.md). The rest of this page starts from a finished `infected.vpk`.

## Three steps

1. **Install the app.** Copy `infected.vpk` to the Vita (USB or FTP), open it in VitaShell and press **CROSS**. An **Infected** bubble
   appears on the home screen (original artwork by default, or pictures made from your own disc: `docs/BUILDING.md`).
2. **Add your game.** Copy your Infected ISO into **`ux0:data/infected/`**. Any file name that ends in `.iso` works.
   (If the folder is not there yet, start the app once. It makes the folder and tells you what is missing.)
3. **Play.** Open the bubble.

That is all.

## Before the first start

* The Vita needs **`libshacccg.suprx` in `ur0:data/`**: Sony's shader compiler, taken from your own console with one of the usual
  community installers. Every game that runs on vitaGL needs it, so you may have it already.
* About **600 MB free on `ux0:`** for the ISO, plus 72 MB (2x pack) or 102 MB (4x pack) if you use HD textures.
* The first start takes a little longer: the game prepares its shaders during the opening logos (about 1.5 s on a Vita) and keeps them in
  `ux0:data/infected/shader_cache`, so missions don't pause on them later.

## Optional

* **Sharper textures.** Copy `texpack_2x.bin` (and, if you like, `texpack_4x.bin`, the mixed 4x pack) into `ux0:data/infected/`. You make
  them from your own disc with `tools/texpack/build_packs.sh` (a couple of hours on a PC; see `tools/texpack/README.md`). The game
  picks them up by itself (settings menu, "HD textures").
* **500 MHz.** The Vita's retail limit is 444 MHz. With PSVshellPlus installed, copy `vita/psvshell/INFC00001` to
  `ur0:data/PSVshell/profiles/INFC00001` (see `vita/psvshell/README.md`); the log then says `the ARM core measures 49x MHz`. Without it the game
  runs at 444 MHz. 500 MHz is outside Sony's specification: your choice.
* **RetroFlow.** If you use the RetroFlow launcher, the app puts its cover and background into `ux0:data/RetroFlow/` the first time it
  starts (only if RetroFlow is installed and you have no picture of your own for it). RetroFlow shows them the next time it refreshes
  its list. To use the game's own art instead, see `docs/BUILDING.md` (`art_from_disc.py`, made from your own disc, kept on your machine).

## If something is wrong

The boot screen says what is missing:

| It says | Do this |
| --- | --- |
| *No disc image found* | Step 2: the ISO must be inside `ux0:data/infected/` and end in `.iso`. |
| *not the one this build was generated from* | Your ISO is another version or game. You need the US version, ULUS-10054, 1.02, and the VPK must have been built from that same disc. |
| *libshacccg.suprx not found* | Put Sony's shader compiler, `libshacccg.suprx`, into `ur0:data/`. |

Anything else: play for a minute, leave the game (hold **START + SELECT + L + R** for a second) and look at `ux0:data/infected/log.txt`.
It lists the settings, the clock, the memory, and ends with `clean exit` after a normal run.

## Controls

The Modern layout (default) fires with **R**, locks on with **L** and opens the map with the right stick; [CONTROLS.md](CONTROLS.md) has it all.
Hold **SELECT + START** for half a second (or tap the top-right corner of the touch screen) for the settings menu
([SETTINGS.md](SETTINGS.md)). Hold **START + SELECT + L + R** to leave the game.

## Update, remove

* **Update:** install the new `infected.vpk` over the old one. Your saves and settings are in `ux0:data/infected/`, which the package does not touch.
* **Remove:** delete the bubble in VitaShell. To remove the game's files and saves too, delete `ux0:data/infected/`.
