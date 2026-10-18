# Source Record

Imported on 2026-10-18 from the current prepared source working tree,
including its uncommitted build/HD-tool additions. This is not the older
October 10 snapshot.

| Item | Value |
| --- | --- |
| Source export SHA-256 | `23d4f7be85116377c36a5efc3d5060a62ee17656c7a95d40cad2c10e1ea2b718` |
| PortableKit upstream base | `33800c4060c53f3233e08f8ab2cc4590a0872063` |
| Patched PortableKit tree | `ec7dc6d10df8aca87b77d3d6ee145808c40c2ecb` |
| Recorded local PortableKit revision | `fa16fa7` |
| Current installed eboot SHA-256 | `2e90c943f48fa673f31f48c12872b4231988b20c9690e2dee291fad89040aceb` |
| Private candidate VPK SHA-256 | `4ba3b98ede00f87a7e830f61876e65d2d0f3508076194471b705571d140cc0cc` |
| Title ID | `INFC00001` |

The original working log ties the installed executable to the completed
October 17 build. A later loose speed-test executable was not selected.
That record is not a byte-for-byte clean-rebuild result. The new Git commit
identifies this reviewed source snapshot, not the original private history.

Included: platform/runtime source, recompiler, all 64 PortableKit patches,
pinned fetch/build scripts, texture/movie tools and source-level tests.
PortableKit is fetched from its pinned upstream commit rather than vendored
twice. Its resulting patched tree was independently checked on 2026-10-18.

Excluded: ISO, extracted/translated game code, VPK, proprietary modules,
weights, textures/movies, captures, device profiles and private logs/history.
Current guides replace the old lab notes. Owner-specific names/paths were
normalized without intentionally changing game behavior.

[SOURCE-MANIFEST.json](SOURCE-MANIFEST.json) hashes the imported files.
[Building](BUILDING.md) lists the exact recipe and remaining verification.
