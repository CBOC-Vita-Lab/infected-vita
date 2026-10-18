# Infected Vita

A native PS Vita runtime for Infected. You supply your own supported PSP
game dump; the game is not included.

## Install

The normal setup is a VPK, your ISO and the required shader compiler module:

1. Install the matching `infected.vpk` with VitaShell.
2. Copy your own supported ISO to `ux0:data/infected/Infected.iso`.
3. Open the Infected bubble.

See [Install](INSTALL.md) for requirements, backups and troubleshooting.
HD textures are optional. **HDcube is used on the computer to prepare them,
not installed on the Vita.** [HD Textures](HD-TEXTURES.md) explains the model
download and where generated packs belong.

## Release Status

This repository is staying private. The recovered installed runtime matches
the current package, and its retail artwork is kept out of shared files.
The corresponding current source, preparation tools and clean-build record
are still missing, so this is not a released installer or a fully verified
install guide. No old source or loose speed-test executable is being used
as a substitute.

The current default-settings and HD workflows still need broader Vita
testing. No whole-game completion or locked-30-FPS claim is made.

[Included Files](ASSETS.md) | [Credits](CREDITS.md)

CBOC, Claude and ChatGPT contributed to the port and release preparation;
upstream contributors retain their credit and rights.
