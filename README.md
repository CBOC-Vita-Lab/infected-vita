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

## Included

Source, the recompiler, dependency patches, HD preparation tools, install
guides and a download checker are together in this repository. The texture
builder fetches its models directly from their authors; no manual model
hunting is needed.

This is a private preview. The existing owner's ready-made VPK contains
translated game code and cannot be shared as the public installer. New
owners use their own disc with [Building](BUILDING.md). Build and test details
are kept in [Source Record](SOURCE.md) and [Known Issues](KNOWN-ISSUES.md).

[Controls](CONTROLS.md) | [Building](BUILDING.md) | [Source Record](SOURCE.md)
| [Technical Notes](TECHNICAL.md) | [Known Issues](KNOWN-ISSUES.md)
| [Included Files](ASSETS.md) | [Licenses](THIRD_PARTY_NOTICES.md)
| [Changes](CHANGELOG.md) | [Credits](CREDITS.md)

CBOC, Claude and ChatGPT contributed to the port and release preparation;
upstream contributors retain their credit and rights.
