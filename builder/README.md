# The Infected builder

`infected-build` turns a player's own Infected disc image into `infected.vpk` on a computer that has nothing installed: no VitaSDK,
no compiler, no Python, no `make`. It is one program (Go, standard library only, no cgo) plus a **builder kit**, a folder with a
trimmed VitaSDK and the parts of the application that do not depend on the disc. Nothing derived from the game is ever shipped:
the game's code is translated from the player's disc, on the player's computer, every time.

For players: [`docs/BUILDING.md`](../docs/BUILDING.md), "The easy way". This page is for people who change the builder.

```
 Infected.iso
   │  psp.ReadFile / psp.BuildImage        BOOT.BIN (SHA-256 checked) and the relocated image         builder/psp
   │  leangen.Generate                     74 C++ files, byte-identical to recomp/lean_gen.py          builder/leangen
   ▼
 work/gen/lean_NNN.cpp ──kit g++ -c (N parallel)──► work/obj/*.o ──kit gcc-ar──► liblean_vita_lto.a
   ▼
 kit g++ -flto (link)  game/link/obj/*.o + game/link/lib/*.a + liblean_vita_lto.a  ──►  infected.elf
   ▼  nm check, text-pad check
 vita-elf-create ► vita-make-fself ► eboot.bin ──zip with game/vpk/*──► infected.vpk              builder/cmd/infected-build
```

## Using it by hand

```
infected-build [--out DIR] [--jobs N] [--work DIR] [--progress] [--keep] [--kit DIR] [--verify] Infected.iso
infected-build --gen DIR [--out DIR] [--work DIR] ...        (no disc: build from C++ that is already generated)
```

| | |
| --- | --- |
| `--out DIR` | where `infected.vpk` goes (default: the folder of the .iso) |
| `--jobs N` | parallel compiles and LTO partitions (default `min(CPUs, free RAM / 1.5 GB)`, at least 1) |
| `--work DIR` | work folder (default: a new short folder in the temporary folder). Only what the builder made in it is deleted. |
| `--progress` | machine-readable output on stdout, see below |
| `--keep` | keep the work folder: `build.log`, the generated C++, objects, archive, ELF, `eboot.bin` |
| `--kit DIR` | the kit folder (default: the folder of the program) |
| `--gen DIR` | skip the disc and the translation: compile the `lean_*.cpp`, `lean_table.cpp` and `lean_decls.h` in DIR (what CI does with a synthetic corpus) |
| `--verify` | check the SHA-256 of every kit file first (otherwise only that every file exists with its size) |

Started without arguments (a double-click) it asks you to drag the .iso onto the window and waits for Enter at the end.
Exit status: 0 success, 1 failure (one plain-English line on stderr), 2 bad command line, 130 cancelled (Ctrl-C or SIGTERM; child
processes are killed and the work folder is removed). The tools run as a process tree (gcc starts cc1plus, lto1, as, ld and this
program as make), so cancelling kills the whole tree: a process group on Linux and macOS, `taskkill /T /F` on Windows. On Windows
the program also puts itself into a job object that kills every process in it when the program ends in any way (window closed, the
installer ends it with TerminateProcess, a crash), so no compiler is left running without its parent.

`--progress` lines (anything else on stdout is ignored by readers):

```
@step Reading your disc | Translating the game | Compiling (this is the long part) | Linking | Packaging
@progress <done> <total>      within the step (translating: percent-like units; compiling: units done of 74; linking: LTO partitions done)
@done /absolute/path/infected.vpk
```

All output of the tools goes to `<work>/build.log`. After a failure that file is kept and its path is in the message; after a success the
work folder is deleted (unless `--keep`). Needs about 700 MB of free disk space in the work folder (`kit.json` `minDiskBytes`).

macOS older than 14 is refused up front (the VitaSDK compilers of the kit need 14); `kit.json` `requires.macos` can raise that.

## What is in a kit

```
infected-build[.exe]      this program (added by `make-kit.sh pack --exe`; not listed in kit.json "files")
kit.json                  the recipe (below); first entry of the archive
toolchain/                trimmed VitaSDK: the host programs (bin/arm-vita-eabi-g++, -gcc-ar, -nm, -readelf, vita-elf-create, vita-make-fself,
                          libexec/.../cc1plus, lto1, lto-wrapper, collect2, plugins, as, ld, ar) and the target files they open
                          (arm-vita-eabi/include and lib, crt*.o, libgcc.a, libstdc++, linker scripts); lists/host-files.txt and lists/target-files.txt
game/include/             headers the generated C++ includes (psprecomp/*, lean_abi.h, lean_vfpu*.h)
game/profile.json         recomp/infected_profile.json
game/link/                disc-independent objects of the application (obj/), static libraries (lib/), link.args (the link command)
game/vpk/                 every file of the package except eboot.bin (sce_sys, module/infected_core3.skprx, shader_cache/, retroflow/ ...)
licenses/                 GPL/LGPL/MIT texts and SOURCES.txt: where every part comes from
```

The published archive is `Infected-Vita-builder-<os>.tar.gz` (`linux`, `windows`, `mac-intel`, `mac-arm`), flat (no top folder), `kit.json` first,
file modes kept (executables 755), reproducible (fixed times and owner).

### kit.json (format 1)

The program reads these fields; unknown fields are ignored. Every command runs with the **kit folder** as the working directory except
the compiles, which run in the **work folder** (see Determinism). Tool names are keys of `tools`; their paths are relative to the kit.

| field | meaning |
| --- | --- |
| `game.bootSha256` | SHA-256 of the BOOT.BIN this kit builds (Infected ULUS-10054 1.02) |
| `requires.macos` | lowest macOS major version (default 14) |
| `tools` | `cxx`, `ar`, `nm`, `readelf`, `elfCreate`, `fself`: path of the program (with `.exe` in a windows kit) |
| `profile` | the generator profile (`game/profile.json`) |
| `compile` | `args` for the C++ compiler; `sources` patterns in `work/gen` (`lean_[0-9]*.cpp`, `lean_table.cpp`) |
| `archive` | `args` (`rcs {lib} {objs}`) and `order`: the object patterns in `work/obj`, in that order |
| `link` | `argsFile` (`game/link/link.args`, one argument per line) and `extraArgs` appended to it (`-ffile-prefix-map=...`, `-g0`) |
| `postLinkCheck` | run `nm`; fail if a line matches `failIfLineMatches` (weak pthread functions) |
| `textPad` | `object` (the text-pad object in link.args), `minFreeBytes`, `check` (readelf), `fix` (how to recompile the pad object when the code segment leaves too little room, then link again) |
| `elfCreate`, `fself` | arguments of `vita-elf-create` and `vita-make-fself`; `elfCreate.stripSections` (see Determinism) |
| `vpk` | the output name and the ordered `entries`; every entry but `eboot` comes from `game/vpk/` |
| `minDiskBytes` | free space to require in the work folder |
| `files` | every file under `game/`, `toolchain/`, `licenses/` with `sha256` and `size` |

Placeholders in `args`: `{stem}` (a generated file name without `.cpp`; also the fixed `-frandom-seed`), `{kit}`, `{work}` (absolute folders),
`{lib}`/`{lean}` (the game archive), `{objs}` (all objects, as separate arguments), `{out}` (linked ELF), `{jobs}`,
`{elf}`, `{velf}`, `{eboot}`, `{pad}`, `{padObject}`. A placeholder the program does not know is an error ("needs a newer infected-build").

### The tools never see your environment

The builder starts every tool with an environment it builds itself: `PATH` is the kit's `toolchain/bin`, `TMPDIR`/`TEMP`/`TMP` are the
work folder's `tmp/`, `LC_ALL=C`, plus `SystemRoot` and friends on Windows. `VITASDK`, the user's `PATH` and the rest are not passed on.
GCC finds its parts relative to its own location.

## Parallel LTO link without `make`

The final link is an LTO link: whole-program analysis runs once (serial, about half of the time), then the program is cut into partitions
that are compiled in parallel. GCC's `lto-wrapper` does the second half by writing a makefile and running
`$MAKE -f /tmp/ccXXXX.mk -jN all` (it first runs `$MAKE --version`); without a `make` it compiles the partitions one after the other. The player's computer
has no `make`, so the builder sets `MAKE` to **itself** and `INFECTED_BUILD_MAKE=1`; when it is started with that variable it
behaves as make (`make.go`). It understands exactly what GCC 15 writes: rules `target:` + tab + `@command`, `.PHONY: all`, `all:` with
continued prerequisite lines, `-f`, `-j N`/`-jN`, `-C`, `--version`, quoting, `&&`, `> /dev/null`, and the `touch -r`/`mv` of older GCCs. Every rule
that has a recipe runs, in parallel up to N, and the first failure stops the rest (exit 2, `make: *** [target] Error N`). It writes
"done total" to `tmp/partitions.txt` after each partition, which becomes the `@progress` of the Linking step.
Other recipes (pipes and so on) go to `/bin/sh -c` where there is one.

Measured on a 2-core machine, same objects: serial link 616 s, with the built-in make about 320 s (two `lto1` run at once, checked by
sampling the process list). Linking the same objects with GNU make instead (`MAKE=/usr/bin/make`, 318 s) gives a byte-identical ELF and `eboot.bin`.

**A space in the path of `$MAKE` makes lto-wrapper skip make silently** (it links serially and says nothing), and players' folders have spaces
(`C:\Users\John Smith\...`). So the builder passes `MAKE` as its own path when that has no space, else `./infected-build` (relative to the kit
folder, where the link runs), else a copy in a space-free temporary folder, else the 8.3 short name (Windows). The same goes for the
temporary folder (the makefile names its targets there unquoted): `<work>/tmp`, or a space-free folder elsewhere when the work folder
has a space (`runner.go`: `makeProgram`, `spacelessDir`).

## Determinism: the same eboot from any folder

GCC streams file names and directories into LTO objects, and the final link orders some branch stubs by them, so the same sources used to
give an `eboot.bin` that differed by a few hundred bytes between folders. The kit therefore:

- compiles in the work folder with **relative** names (`-Iinc -Igen gen/lean_000.cpp -o obj/lean_000.o`), the same on every machine;
- passes `-ffile-prefix-map={kit}=/kit -ffile-prefix-map={work}=/work` to the compiles and the link (`compile.args`, `link.extraArgs`);
- fixes the seed GCC derives its internal names from: `-frandom-seed={stem}` per unit, `-frandom-seed=infected` at the link;
- feeds the objects and the archive in sorted order.

What did not work: `-ffile-prefix-map`/`-fdebug-prefix-map` do not reach everything. GCC 15 streams the **absolute working directory** into
every LTO object (it sits in the declaration stream next to the source name) and no prefix map rewrites it. It takes that string from `$PWD`
when `$PWD` names the working directory (it checks device and inode), so on **Linux** the builder sets `PWD=/proc/self/cwd` for every tool:
the string then reads the same on every machine. (The kit location needed only the prefix map: the toolchain's system headers are
streamed with their absolute path.) The link uses `-g0`: the `-g` that CMake puts into `link.args` makes the LTO link write DWARF with
folder names in it, and the generated code itself depends on `-g` (a few hundred bytes of branch stubs).

Result on Linux (2 cores, GCC 15.2.0 kit): `eboot.bin` SHA-256
`493a3f2755d523e467019f4660a7a1d4fad7b47e443e77aebccf3da913879cb2` from a kit in `.../ku1` and an automatic work folder, and from a copy of the
kit in `.../ku2 (b)/with space` with `--work` elsewhere and `--gen`, both under `env -i`; `infected.vpk` is byte-identical too. The same held for the link
without `-g0`. On **macOS and Windows** nothing like `/proc/self/cwd` was found, so the working directory still ends up in the objects there: the
`eboot.bin` can differ between work folders by some hundred bytes of branch-stub order (nothing else, and nothing that matters on the Vita);
this was not measured on those systems. Use the same `--work` folder on the same system when you compare builds.

### The same eboot.bin on every computer (Linux, Windows, both Macs)

CI builds the synthetic corpus (`.github/smoke/gen`, no game data) with the unpacked kit of each computer through `infected-build --gen`, and the
`compare` job of `kits.yml` fails unless the four `eboot.bin` are byte for byte the same. Two things had to be right for Windows:

- **`-ffile-prefix-map` is text.** GCC on Windows spells its own paths with forward slashes (`D:/a/_temp/smoke test/the kit/toolchain/bin/../lib/...`), so
  the builder expands `{kit}` and `{work}` with forward slashes; a map written with backslashes matches nothing there. (The working
  directory that GCC streams into the LTO objects is not mapped on Windows; there, two work folders of different length gave the same
  `eboot.bin` in the test.)
- **The Sony ELF is stripped to what the Vita loads.** `vita-make-fself` computes the module NID of the program from a hash of the *whole* Sony
  ELF file, not only of the segments, and that file carried the linker's symbol table. `ld` writes the global symbols in the order of its hash
  table (`bfd_hash_hash`, table size 16381), and its hash is an `unsigned long`: 64 bits on Linux and macOS, 32 bits in the Windows build.
  Symbols with long C++ names (mostly `std::_Hashtable<...>` instantiations) get another bucket there, so `.symtab`, `.strtab` and `.rel.*` differ
  (29 of 10,180 global symbols moved in the test), the NID differs, and so does `eboot.bin` (4 bytes of the code segment, its compressed tail and
  the header digest), although every loadable segment is the same. `kit.json` therefore says `"stripSections": true` in `elfCreate`, and the program
  (`velf.go`) keeps the ELF header, the program headers and the segments of the `.velf` and zeroes the rest (section header fields included) before
  `vita-make-fself` runs. (`vita-elf-create -s` strips the file too, but the Windows build of it ignores `-s`.) Nothing the Vita loads changes: the
  `eboot.bin` made with a stripped Sony ELF differs from the one made with the full file in the module NID and the header digest only.

## Making a kit

`builder/kit/make-kit.sh` (Linux, needs a full VitaSDK, the third-party sysroot of `vita/scripts/build-sysroot.sh`, and a
PortableKit made by `scripts/get-portablekit.sh`). The kit is made in two parts so that CI can build the target part once and
the host part once per host system:

```sh
# target part: toolchain target files, game/*, kit recipe (no disc is involved; the game library is a stub)
builder/kit/make-kit.sh target --sdk $VITASDK --sysroot vita/sysroot --repo . --out /tmp/kit-target \
    [--shader-cache DIR]            # a vitaGL shader cache (the folder with v3/) to ship in the package
# host part: the programs that run on the player's computer, copied from a VitaSDK tree of that system
builder/kit/make-kit.sh host --target-dir /tmp/kit-target --out /tmp/kit-linux --host-sdk $VITASDK_FOR_THAT_OS \
    --host-os linux|windows|mac --host-arch amd64|arm64
# the archive (put the built program into the kit first)
builder/kit/make-kit.sh pack --kit /tmp/kit-linux --exe infected-build --out Infected-Vita-builder-linux.tar.gz
```

`make-kit.sh all` does target + host (this Linux) + pack in one go. `make-kit.sh trace` re-derives `lists/*.txt` by running a build under
`strace`. Without `--build-dir` the target part builds the application objects with `vita/app/build.sh` and a stub game library, with the
generated default bubble art (never anyone's personal art). `builder/kit/build-with-kit.sh` is the shell version of the builder, kept as the
readable reference of the steps.

## Tests

```sh
cd builder
go vet ./... && go test ./...          # no disc needed
GOOS=windows GOARCH=amd64 go build -o /dev/null ./cmd/infected-build     # also darwin/amd64, darwin/arm64, linux/amd64
```

`leangen/ref_test.go` compares the translator against the Python generator when `INFECTED_ISO` and `INFECTED_REF` are set. The end-to-end test is
running `infected-build --kit KIT Infected.iso` in `env -i` and comparing the VPK file list with a normal build.
