# Credits

## Who made this

**The code and the documents were written by Claude**, Anthropic's AI model (the Opus 5.5 and Sonnet 5.5 models that the commit
trailers name), working through Claude Code. The project is fully vibecoded.

A person directed it the whole way, in conversation: chose the game, supplied the console and the disc, made the console available for
each test and said what to do next. Every run on a console was on that person's own Vita; the numbers in `docs/STATUS.md` come from the
logs of those runs.

"Written by Claude" has limits. These parts are somebody else's work:

| What | By | Licence | Where it is here |
| --- | --- | --- | --- |
| [PortableKit](https://github.com/TeamGDB/PortableKit): the recompiler, the PSP system re-implementation, the renderer, the settings and installer code | the PortableKit contributors (TeamGDB) | MIT | Not in this repository. `scripts/get-portablekit.sh` fetches it; `patches/portablekit/` are the changes made for this port |
| MMPX, the pixel-art upscaler used for the HUD (`host/gpu/mmpx.*` and its test) | taken from [Yakumo](https://github.com/TeamGDB/Yakumo) (MIT); the method is Morgan McGuire and Mara Gagiu's, *MMPX Style-Preserving Pixel Art Magnification*, JCGT 10(2), 2021 | MIT | In one of the PortableKit patches; credited in PortableKit's `docs/SOURCE_PROVENANCE.md` |
| [vitaGL](https://github.com/Rinnegatamante/vitaGL), vitaShaRK, SceShaccCgExt, math-neon | Rinnegatamante and contributors | vitaGL: GPL-3.0; the others: see their repositories | Fetched by `vita/scripts/fetch-third-party.sh` at pinned commits; four small patches to vitaGL are in `vita/scripts/patches/` |
| [FFmpeg](https://ffmpeg.org/) 7.1.5 (ATRAC3, ATRAC3plus and H.264 decoders only) | the FFmpeg developers | LGPL-2.1-or-later | Fetched and built by `vita/scripts/build-ffmpeg.sh`, with one small patch |
| The core-3 unlock (`vita/core3/`): which ThreadMgr function to hook, and where | [CapUnlocker](https://github.com/GrapheneCt/CapUnlocker) by GrapheneCt | MIT | Our own module, written from that fact; it loads only on firmware 3.60 to 3.74 |
| font8x8 (boot screen text) | Daniel Hepper | public domain | Fetched by `vita/scripts/fetch-third-party.sh` |
| [VitaSDK](https://vitasdk.org/), taiHEN | the VitaSDK team, Yifan Lu and contributors | see their repositories | The toolchain and stubs the build uses |

## What it runs with

[HENkaku](https://henkaku.xyz/) and Ensō (custom firmware), [VitaShell](https://github.com/TheOfficialFloW/VitaShell),
[vitacompanion](https://github.com/devnoname120/vitacompanion) (used to install and drive test runs over the network),
PSVshell and PSVshellPlus (the 500 MHz profile), [RetroFlow](https://github.com/jimbob4000/RetroFlow-Launcher) (the launcher the cover art is for),
and [Vita3K](https://vita3k.org/) (the emulator used for the first runs).

## The game

*Infected* (2005) was developed by Planet Moon Studios and published by Majesco Entertainment. This project is not affiliated with
or endorsed by them, by Sony Interactive Entertainment, or by any other rights holder. It contains no part of the game, and the
pictures in this repository show it only to demonstrate the port. All rights in the game and its artwork belong to their owners.
