# Technical Notes

`recomp/` translates the supported PSP executable into C++ on the owner's
computer. `port/` supplies game-specific integration. `vita/app/` and
`vita/platform/` provide the Vita runtime. PortableKit is fetched and patched
by `scripts/get-portablekit.sh`; its renderer and HLE changes are preserved
in `patches/portablekit/`.

The recorded runtime uses split rendering, compact vertices and the patched
vitaGL path. Original data stays in `ux0:data/infected/`. Saves are under
`ms0/`; settings are in `settings.txt`. Never replace those with lab fixtures.

HD textures are computer-generated packs keyed to the game's texture data.
Automatic mode prefers the 2x pack. The mixed pack is different from the
large all-4x pack. HD movies are experimental and not needed for normal play.

[Architecture](docs/ARCHITECTURE.md), [Graphics](docs/GL_RENDERER.md) and
[Settings](docs/SETTINGS.md) contain source-level detail. Historical figures
in those reference notes are not acceptance results for this package.
