# Third-party Notices

[LICENSE](LICENSE) preserves the original MIT grant and its exceptions.
This import does not assign ownership of upstream code to the publisher.

| Component | Terms / source |
| --- | --- |
| Port and PortableKit patches | MIT; copyright notices retained |
| PortableKit | [TeamGDB/PortableKit](https://github.com/TeamGDB/PortableKit), pinned by `scripts/get-portablekit.sh`; preserve its LICENSE and third-party notices after fetching |
| vitaGL and local patches | GPL-3.0; `licenses/GPL-3.0.txt` and file notices |
| FFmpeg and local patch | LGPL-2.1-or-later for the selected configuration; fetched source contains the full terms |
| Other graphics/audio dependencies | Pinned in `vita/scripts/fetch-third-party.sh`; retain their source notices when packaging |
| HDcube model | Separate restricted model license; obtained from the author, not bundled |
| Real-ESRGAN and encoding tools | See [Texture Tools](tools/texpack/README.md) for upstream links and terms |
| File checker | `CHECK-FILES-LICENSE.txt`, not a license for the game/runtime |

The generated game library and final VPK contain translated retail code.
The port's MIT license does not permit distributing that game code. Disc
artwork, screenshots, texture packs and movies are separate from code licenses.
No new binary release is being distributed by this source import.
