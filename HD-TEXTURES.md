# Optional HD Textures

Skip this page for normal installation. HDcube is a **computer-side texture
model**, not Vita software. Do not copy its `.pth` file to the console.

## Get The Model Separately

1. Open the author's
   [4xHDCube4Compact model page](https://github.com/Venomalia/HDcube/blob/main/v4Compact/4xHDCube4Compact.pth).
2. Use **Download raw file** and keep the filename
   **`4xHDCube4Compact.pth`** on your computer. Do not save the GitHub web page
   as the model.
3. Claude's recovered texture builder uses
   `texpack-work/models/4xHDCube4Compact.pth` by default. Supply your own
   supported ISO to `proj/tools/texpack/build_packs.sh`; its model-fetching
   step obtains the required models directly from their authors.

The builder and its guide have been recovered from the October 10 source
bundle. They are being checked against the current runtime before inclusion
here; this draft does not yet supply a verified one-click HD builder.

Permission to bundle the model has been
[requested](https://github.com/Venomalia/HDcube/issues/5), not granted. Once
permission is granted and its conditions are met, we will include it with
the preparation package to make setup easier. Until then, obtain it directly
from the author and follow the [model license](https://github.com/Venomalia/HDcube/blob/main/LICENSE).

## Add A Prepared Pack To The Vita

1. Exit Infected and back up any existing pack/settings.
2. Copy your compatible generated pack into **`ux0:data/infected/`**, keeping
   its exact name: `texpack_2x.bin`, or `texpack_4x.bin` for the mixed Vita pack.
3. Reopen Infected and check its HD texture setting. The recovered console
   uses `gfx.hd_textures=auto`; its log confirms a 2x pack loads. Selection of
   the mixed pack and the current preparation workflow still need testing.

The mixed Vita pack uses 4x world/effects and 2x characters/UI. It is not the
full-size all-4x pack. Do not substitute the latter: it has much higher
memory requirements.

Packs and HD movies derived from the original game are not included. Model
permission alone would not grant rights to redistribute those game assets.
Apple Silicon generation and HD movie playback remain unverified.
