# tools

Most of this directory is development tooling. It was written for one machine (a shared Linux box that held the disc image, the game's
private files and several checkouts) and many scripts assume that layout: `/work/...` paths, a workspace with `private/`, `PortableKit/` next to the
repository. Read a script before running it. Only the first three groups are meant for anyone else.

| What | Where | Needs |
| --- | --- | --- |
| Get `BOOT.BIN` out of your ISO, relocate it for the recompiler | `iso_get.py`, `dump_image.cpp` | [`docs/BUILDING.md`](../docs/BUILDING.md), section 3 |
| HD texture packs from your disc | `texpack/` (start with [`texpack/README.md`](texpack/README.md)) | Python packages and hours of compute; self-contained |
| HD versions of the game's movies (experimental) | `hdmovies/` ([`docs/HD_MOVIES.md`](../docs/HD_MOVIES.md)) | likewise |
| The play-through bot and the replay check | `bot/` ([`bot/README.md`](bot/README.md)) | a desktop build of the port and a profile made from your disc; written for the development layout |
| Console test drivers and result parsers | `hw/`, `rc/`, `all_scripts/`, `all_live.sh`, `gfx_live.sh` | the development layout, and a Vita reachable by vitacompanion |
| Sweeps and benchmarks of the gameplay tuning | `tune/`, `minimap_*`, `*.script`, `phases_*.txt` | the development layout |
| Small helpers | `dis.sh` (disassemble guest code), `hotfuncs.py`, `sheet.py` (contact sheets), `imports.tsv` (the game's PSP imports), `shader_variants/` | |

Nothing derived from the disc belongs in a commit, an issue or a pull request: the packs, extracted textures, upscaled pictures, dumps and the
generated C++ are the game's content.
