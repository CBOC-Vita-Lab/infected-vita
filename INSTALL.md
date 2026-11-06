# Install Infected on a PS Vita

You need a Vita (or PS TV) with HENkaku or ensō and VitaShell, and your own copy of the PSP game **Infected** (US version, ULUS-10054,
1.02) as an `.iso` file. The easy way is the **CBOC Vita Installer**: it builds the game from your disc on your computer and installs it on your Vita.

**There is no VPK to download.** The package contains code translated from the game, so it cannot be shared. The installer (or `infected-build`)
makes it from your own disc, once, on your own computer. Nothing is uploaded.

A hacked Vita is the one thing to sort out first: [HENkaku](https://henkaku.xyz/) or Ensō, and [VitaShell](https://github.com/TheOfficialFloW/VitaShell/releases). The guide at
[vita.hacks.guide](https://vita.hacks.guide/) walks through it for every firmware.

## Get your disc as an ISO

A Vita has no UMD drive, so copy the disc with a PSP 1000, 2000 or 3000 that runs custom firmware such as [ARK](https://github.com/PSP-Archive/ARK-4):

1. Put the UMD in the PSP and connect it to your computer with a USB cable.
2. On the PSP home screen, press **SELECT** to open the VSH menu and set **USB Device** to **UMD Disc**.
3. In **Settings**, open **USB Connection**.
4. Your computer shows the disc as a drive holding one `.iso`. Copy it to your computer.

Only the US release works: ULUS-10054, version 1.02. CSO and PBP files are not supported.

## The easy way: CBOC Vita Installer

You need Windows 64-bit, macOS (10.13 or newer on an Intel Mac, 11 or newer on Apple silicon) or Linux 64-bit, and about 1 GB of free disk space while it builds.

1. Get the [CBOC Vita Installer](https://github.com/CBOC-Vita-Lab/vita-installer/releases/latest) (version 2.0 or newer) and open it.
2. On your Vita, open VitaShell and press **SELECT**. The installer finds it by itself.
3. Pick **Infected** and choose your `.iso`. The installer checks it as soon as you pick it: a wrong version, a damaged copy or a CSO/PBP file is reported on the spot.
4. Press **Install**, then **Allow**. The installer downloads the builder (40-50 MB, once), builds the game in about 10 minutes (9 to 9.5 minutes on a slow 2-core PC, faster on most computers) and sends it to the Vita.
5. Open the **Infected** bubble on the home screen. Your ISO is already in `ux0:data/infected/`.

It adds the optional HD packs too (choose `texpack_2x.bin` and, if you like, `texpack_4x_mixed.bin`; see below) and offers **Prepare your Vita** for the shader compiler (see "Before the first start").

Other computers (32-bit or ARM Windows, ARM Linux) can't build the game. Build it on another machine (next section) and choose the finished `infected.vpk` in the installer.

## Another way: `infected-build`

The release page has the same builder for each system, `Infected-Vita-builder-<your system>.tar.gz`. Unpack it and run `infected-build Infected.iso`
(on Windows, drag the `.iso` onto `infected-build.exe`). It writes `infected.vpk` next to the ISO, and every system makes the byte-identical game.

1. **Install the app.** Copy `infected.vpk` to the Vita (USB or FTP), open it in VitaShell and press **CROSS**. An **Infected** bubble appears on the home screen.
2. **Add your game.** Copy your Infected ISO into **`ux0:data/infected/`**. Any file name that ends in `.iso` works.
   (If the folder is not there yet, start the app once. It makes the folder and tells you what is missing.)
3. **Play.** Open the bubble.

Developers who want to build everything by hand: [`docs/BUILDING.md`](docs/BUILDING.md) (the manual route, for developers).

## Before the first start

* **The shader compiler is optional.** `libshacccg.suprx` in `ur0:data/` is Sony's shader compiler, taken from your own console. The game ships the shaders
  it uses, so it runs without it; with it, a few rarely used effects are also drawn. The usual way to get it is
  [ShaRKBR33D](https://github.com/Rinnegatamante/ShaRKBR33D), a free Vita app: install it, open it once and follow it (**it needs Wi-Fi on the Vita**), and it puts the file in place.
  In the installer, **Prepare your Vita** installs ShaRKBR33D for you. You may have the file already.
* About **600 MB free on `ux0:`** for the ISO, plus 72 MB (2x pack) or 102 MB (4x pack) if you use HD textures.
* The first start takes a little longer: the game prepares its shaders during the opening logos (about 1.5 s on a Vita) and keeps them in
  `ux0:data/infected/shader_cache`, so missions don't pause on them later.

## Optional

* **Sharper textures (optional, and slow to make).** You make the packs from your own disc with `tools/texpack/build_packs.sh`, which takes hours on a PC
  ([HD-TEXTURES.md](HD-TEXTURES.md)). It writes `texpack_2x.bin` and `texpack_4x_mixed.bin`. In the installer, choose those files. By hand, copy `texpack_2x.bin`
  as it is, and copy the mixed pack **under the name `texpack_4x.bin`**, into `ux0:data/infected/`. (The big all-4x pack the tool also writes does not fit the Vita: don't use it.)
  The game picks the packs up by itself (settings menu, "HD textures").
* **500 MHz.** The Vita's retail limit is 444 MHz. With PSVshellPlus installed (you install it yourself) the game sets up its own 500 MHz profile in
  `ur0:data/PSVshell/profiles/INFC00001` while the CPU clock setting is 500 MHz; PSVshellPlus applies it when the game starts, so the first start after
  installing still runs at 444 and the menu says `500 MHz from the next start`. The menu shows the clock it measured. A profile of your own for this game is never overwritten
  (`vita/psvshell/README.md`). Without PSVshellPlus the row reads `444 MHz (500 needs PSVshellPlus)`.
  500 MHz is outside Sony's specification: your choice.
* **RetroFlow.** If you use the RetroFlow launcher, the app puts its cover and background into `ux0:data/RetroFlow/` the first time it
  starts (only if RetroFlow is installed and you have no picture of your own for it). RetroFlow shows them the next time it refreshes
  its list. The installer can also use your own pictures for the bubble and background; developers can build with their own (`docs/BUILDING.md`).

## If something is wrong

The boot screen says what is missing:

| It says | Do this |
| --- | --- |
| *No disc image found* | The ISO must be inside `ux0:data/infected/` and end in `.iso`. Send it again with the installer, or copy it in VitaShell. |
| *not the one this build was generated from* | Your ISO is another version or game. You need the US version, ULUS-10054, 1.02, and the VPK must have been built from that same disc. |
| *No shader compiler. Optional...* | Nothing is wrong: the game runs without it. For the rarely used effects, install it with ShaRKBR33D (needs Wi-Fi on the Vita). |
| *Low memory* or *Low video memory* | Close other apps or restart the Vita, then start the game again. |

Anything else: play for a minute, leave the game (hold **START + SELECT + L + R** for a second) and look at `ux0:data/infected/log.txt`.
It lists the settings, the clock, the memory, and ends with `clean exit` after a normal run.

## Controls

The Modern layout (default) fires with **R**, locks on with **L** and opens the map with the right stick; [CONTROLS.md](CONTROLS.md) has it all.
Hold **SELECT + START** for half a second (or tap the top-right corner of the touch screen) for the settings menu
([SETTINGS.md](SETTINGS.md)). Hold **START + SELECT + L + R** to leave the game.

## Update, remove

* **Update:** install the new `infected.vpk` over the old one. Your saves and settings are in `ux0:data/infected/`, which the package does not touch.
* **Remove:** delete the bubble in VitaShell. To remove the game's files and saves too, delete `ux0:data/infected/`.
