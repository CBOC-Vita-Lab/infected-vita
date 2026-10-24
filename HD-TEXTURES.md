# Optional HD Textures

Skip this page for normal installation. HDcube is a **computer-side texture
model**, not Vita software. Do not copy its `.pth` file to the console.

## Easiest Method

The texture builder is included. First prepare PortableKit using
[Building](docs/BUILDING.md), then run this from the repository/package root:

```sh
tools/texpack/build_packs.sh /path/to/Infected.iso
```

It sets up its Python environment, downloads the required models directly
from their authors, verifies their checksums and writes to
`texpack-work/packs/`. You do not need to download the model by hand.
Processing can take hours; [Texture Tools](tools/texpack/README.md) lists
computer dependencies and explains resuming.

## Manual Model Download

Only use this section if you want to provide the model yourself.

1. Open the author's
   [4xHDCube4Compact model page](https://github.com/Venomalia/HDcube/blob/main/v4Compact/4xHDCube4Compact.pth).
2. Use **Download raw file** and keep the filename
   **`4xHDCube4Compact.pth`** on your computer. Do not save the GitHub web page
   as the model.
3. The texture builder uses
   `texpack-work/models/4xHDCube4Compact.pth` by default. Supply your own
   supported ISO to `tools/texpack/build_packs.sh`; its model-fetching
   step obtains the required models directly from their authors.

Permission to bundle the model has been
[requested](https://github.com/Venomalia/HDcube/issues/5), not granted. Once
permission is granted and its conditions are met, we will include it with
the preparation package to make setup easier. Until then, obtain it directly
from the author and follow the [model license](https://github.com/Venomalia/HDcube/blob/main/LICENSE).

## Add A Prepared Pack To The Vita

1. Exit Infected and back up any existing pack/settings.
2. Copy your compatible generated pack into **`ux0:data/infected/`**, keeping
   its exact name: use generated `texpack_2x.bin` as-is. For the mixed Vita
   pack, copy `texpack_4x_mixed.bin` and name the copy `texpack_4x.bin`.
3. Reopen Infected and check its HD texture setting; use `auto` to let the
   app select an available pack.

The mixed Vita pack uses 4x world/effects and 2x characters/UI. It is not the
full-size all-4x pack. Do not substitute the latter: it has much higher
memory requirements.

Packs and HD movies derived from the original game are not included. Model
permission alone would not grant rights to redistribute those game assets.
Platform and testing notes are in [Known Issues](KNOWN-ISSUES.md).
