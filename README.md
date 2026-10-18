# Infected Vita

A native PS Vita runtime for Infected. You supply your own supported PSP
game dump; the game is not included.

## Install

New to the package? Open [Start Here](START-HERE.txt).

The normal setup is a VPK built from your disc, your ISO and the required
shader compiler module:

1. [Build your VPK](BUILDING.md), then install it with VitaShell.
2. Copy your own supported ISO to `ux0:data/infected/Infected.iso`.
3. Open the Infected bubble.

See [Install](INSTALL.md) for requirements, backups and troubleshooting.
HD textures are optional. **HDcube is used on the computer to prepare them,
not installed on the Vita.** [HD Textures](HD-TEXTURES.md) explains the model
download and where generated packs belong.

## Release Status

This repository is staying private. The recovered installed runtime matches
the current package, and its retail artwork is kept out of shared files.
The current source, recompiler, 64 PortableKit patches and HD preparation
tools are now included. The dependency patch series was applied to its
pinned upstream base and its exact resulting tree verified. The final clean
VPK build and fresh-install test remain pending. No older runtime or loose
speed-test executable is being substituted.

The private ready-made candidate contains translated game code. It is not
a redistributable public download; each owner must generate that code from
their own supported disc. Game-derived HD packs are also not included.

The current default-settings and HD workflows still need broader Vita
testing. No whole-game completion or locked-30-FPS claim is made.

[Controls](CONTROLS.md) | [Building](BUILDING.md) | [Source Record](SOURCE.md)
| [Technical Notes](TECHNICAL.md) | [Known Issues](KNOWN-ISSUES.md)
| [Included Files](ASSETS.md) | [Licenses](THIRD_PARTY_NOTICES.md)
| [Changes](CHANGELOG.md) | [Credits](CREDITS.md)

CBOC, Claude and ChatGPT contributed to the port and release preparation;
upstream contributors retain their credit and rights.
