# LOG - Vita workstream

Short, chronological where it matters. `UNVERIFIED` = could not be checked without a Vita / Vita3K run.
Third-party trees live in `third_party/` and build output in `build/`, `sysroot/`, `dist/`: none of it is committed.

## Environment

* vitasdk at `/work/vitasdk-root/vitasdk` (`scripts/env.sh`): `arm-vita-eabi-gcc` 15.2.0, newlib `2e42829`,
  pthread-embedded `11d2e57`, samples `fe8fbef`, built 2026-09-26. The shared SDK was never modified; our libs/headers are in
  `sysroot/` and are found through explicit `-I`/`-L`/CMake variables.
* cmake 3.28.3, host gcc for the desktop twin, Python 3 with `unicorn` 2.1.4 (`pip install unicorn`), `/usr/bin/ffmpeg` 6.1.1 with libx264
  (only used to generate the T9 streams).
* Machine is shared (2 cores, 7 GB): every build runs under `nice -n 10` with at most `-j2`.
* GCC 15 defaults to gnu23: all third-party C is compiled with `-std=gnu17`. The toolchain defaults to `-fshort-enums`: FFmpeg and the one
  TU that includes its headers (`selftest/t_ffmpeg.cpp`) use `-fno-short-enums`; the link needs `-Wl,--no-enum-size-warning`.

## Pinned sources

| What | URL | Version |
|---|---|---|
| vitaGL | https://github.com/Rinnegatamante/vitaGL | `9bf3e3f4e55efd1f4a191b179144f7f9a98cd925` (2026-10-30, "Avoid setting dirty flags when program doesn't change in glUseProgram.") plus `scripts/patches/vitagl-0001..0004` (was `dca4b9d143290d78ec043131a19be36c78cdc7b5`, 2026-10-07, until 30 Oct; see "30 Oct 2026" below) |
| vitaShaRK | https://github.com/Rinnegatamante/vitaShaRK | `df24065e65098b2d1ac533760109ad4367573f28` |
| SceShaccCgExt | https://github.com/Rinnegatamante/SceShaccCgExt | `102192366c25fe7b8fec073d665494b6172605c3` |
| math-neon | https://github.com/Rinnegatamante/math-neon | `0faab814782c071ff4015527f1ca955ab1ccc470` |
| taiHEN (source, reference only) | https://github.com/yifanlu/taiHEN | `309b3800bcb8ebbd5e4f5e5e920af3da3590b829` (its stub build failed, see below) |
| taiHEN (used) | vdpm package `taihen-0.11-1-vita.pkg.tar.xz` from https://github.com/vitasdk/vitasdk-autobuild/releases/download/packages-2026.08-snapshot-20260925.50.1/ | sha256 `93d2075b6b61a164224a2aad651d3d6787f31f3a19c3e2bada7c99844f9fae90` |
| FFmpeg | https://ffmpeg.org/releases/ffmpeg-7.1.5.tar.xz | sha256 `de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f` |
| font8x8_basic.h | https://raw.githubusercontent.com/dhepper/font8x8/master/font8x8_basic.h (public domain; copied to `selftest/`) | sha256 `49d8df366296b203ca3211bc0672cf2a762135bf12710735b6292756b19dffd5` |
| Vita3K (read-only, for CLI/memory/module facts) | https://github.com/Vita3K/Vita3K | `b75d5e6292d5caa6a8411bacc9db1eca1531beab` (2026-10-09) |
| vitasdk samples (reference only) | https://github.com/vitasdk/samples | `fe8fbef570f3280586c0c20157146e3faefb2181` |

`scripts/fetch-third-party.sh` re-creates `third_party/` at these pins (written from the commands below, not re-run end to end).

## Exact build commands (one-time library setup; order matters)

```sh
cd /work/ws/vita && source scripts/env.sh && mkdir -p sysroot/lib sysroot/include

# 1. taiHEN stubs: prebuilt package. Building the stubs from source failed (the kernel headers in this SDK lack
#    ksceKernelGetProcessTitleId etc.). Verified sha256 above, then:
tar -xJf third_party/vdpm-pkgs/taihen-0.11-1-vita.pkg.tar.xz -C third_party/vdpm-pkgs/x-taihen
cp third_party/vdpm-pkgs/x-taihen/arm-vita-eabi/lib/*.a sysroot/lib/
cp third_party/vdpm-pkgs/x-taihen/arm-vita-eabi/include/taihen.h sysroot/include/

# 2. SceShaccCgExt (needs -std=gnu17: C23 changes the meaning of "()" prototypes)
mkdir -p build/shaccgext && cd build/shaccgext
nice -n 10 cmake -S ../../third_party/SceShaccCgExt -B . -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS="-std=gnu17 -I$VITA_SYSROOT/include" -DCMAKE_CXX_FLAGS="-I$VITA_SYSROOT/include"
nice -n 10 make -j2
cp libSceShaccCgExt.a $VITA_SYSROOT/lib/ && cp ../../third_party/SceShaccCgExt/include/shacccg_ext.h $VITA_SYSROOT/include/
cd ../..

# 3. vitaShaRK (includes shacccg_ext.h from step 2)
cd third_party/vitaShaRK
nice -n 10 make CFLAGS="-g -Wl,-q -O2 -ffast-math -mtune=cortex-a9 -mfpu=neon -ftree-vectorize -std=gnu17 -I$VITA_SYSROOT/include"
cp libvitashark.a $VITA_SYSROOT/lib/ && cp source/vitashark.h $VITA_SYSROOT/include/
cd ../..

# 4. math-neon
(cd third_party/math-neon && nice -n 10 make -j2)
cp third_party/math-neon/libmathneon.a $VITA_SYSROOT/lib/ && cp third_party/math-neon/source/math_neon.h $VITA_SYSROOT/include/

# 5. vitaGL variants (flags: VITAGL_NOTES.md section 1; each variant is built in a fresh copy build/vitagl-<variant>)
for v in release debug fast store; do scripts/build-vitagl.sh $v; done

# 6. FFmpeg (configure line below)
scripts/build-ffmpeg.sh nopt
scripts/build-ffmpeg.sh pt

# 7. The packages
vita/build-all.sh            # or vita/build.sh [--vitagl V] [--ffmpeg V] [--attr2 N] for one flavour
```

FFmpeg configure line (identical for both variants except the threading switch; from `config.h`):

```
--prefix=/ffmpeg-vita --enable-cross-compile --cross-prefix=arm-vita-eabi- --arch=arm --cpu=cortex-a9 --target-os=none
--enable-static --disable-shared --disable-programs --disable-doc --disable-avdevice --disable-avformat --disable-avfilter
--disable-swscale --disable-swresample --disable-postproc --disable-network --disable-autodetect --disable-everything
--enable-decoder=atrac3,atrac3p,h264 --enable-parser=h264 --enable-asm --enable-neon
{nopt: --disable-pthreads --disable-w32threads --disable-os2threads | pt: --enable-pthreads}
--disable-debug --disable-symver --disable-stripping
--extra-cflags='-O3 -mfpu=neon -mfloat-abi=hard -mtune=cortex-a9 -fno-short-enums -fno-math-errno -fno-signed-zeros'
```

plus `scripts/patches/ffmpeg-tx-float-only.patch` (sha256 `7be0cb378a8438dadecfbd0b1dca1e5d5a3281307be2c72068826de14db4ce4d`), applied to
`libavutil/tx.c` by the script: only the float codelet list is linked. Without it `tx_double.o` + `tx_int32.o` pull about 12.6 MiB of static
twiddle-table BSS into the image; with it the whole image BSS is about 6.9 MiB (7,222,248 B) (4 MiB of that are the float tables ATRAC3plus' MDCT uses).

| FFmpeg | `libavcodec.a` | `libavutil.a` |
|---|---|---|
| `nopt` | 1,387,418 B | 770,746 B |
| `pt` | 1,410,754 B | 776,782 B |

Both link into the self-test. `pt` pulls `-lpthread` (pthread-embedded). Whether `pt` works at runtime is UNVERIFIED (use
`dist/variants/infected_selftest-ffmpeg-pt.vpk`); `nopt` is the default.

Library sizes (sysroot): `libvitaGL.a` 6,398,508 B, `_debug` 6,831,756 B, `_fast` 6,348,044 B, `_store` 6,398,070 B (`.flags` files next to them),
`libvitashark.a` 487,998 B, `libSceShaccCgExt.a` 65,918 B, `libmathneon.a` 161,590 B.

## Validation done without a Vita

* `scripts/build-selftest-desktop.sh run`: harness + platform twin on the host: 22 passed, 0 failed (T3/T6-T9 skip there).
* `scripts/check-coro.sh`: the ARM coroutine switch (`selftest/coro_arm.S` + `coro_core.cpp`) assembled with the vitasdk toolchain and executed
  instruction by instruction in Unicorn (star, ring and deep-C++ modes, canary and callee-saved register checks: all OK). `--negative` runs a
  deliberately broken switch (r7 clobbered) and the validator rejects it (modes 0 and 1 FAIL). It found one real bug early (the saved stack
  pointer slot, see below).
* `scripts/gen_t9_streams.py`: generates the synthetic ATRAC3plus-sized blocks and the H.264 stream (`/usr/bin/ffmpeg` + libx264) embedded in
  `selftest/t9_streams.h`.
* Link-level checks: `nm` shows the `sceUserMainThread*` symbols, `_newlib_heap_size_user`, `vgl_hook_printf` in the debug ELF; `unzip -l` / an SFO
  parser show the VPK contents and `ATTRIBUTE2` (12 default, 0 in the `attr2-0` flavour).
* Nothing has run on a Vita or in Vita3K from this workstream.

## Mishaps and workarounds (so nobody repeats them)

* **Overbroad `pkill`**: `pkill -f "make -j2"` killed my own shell (exit 144) and may have interrupted other agents' `make -j2` runs; another
  agent's ninja build was still alive afterwards. From then on only specific PIDs were killed. Do not use pattern kills on this shared machine.
* Copying the taiHEN stubs into the shared SDK's `lib/` was blocked by the permission system: switched to the private `sysroot/`.
* taiHEN source build failed (missing `ksceKernelGetProcessTitleId` and friends in this SDK's kernel headers) -> prebuilt vdpm package, sha256 checked.
* SceShaccCgExt failed with C23 `()` semantics -> `-std=gnu17`. vitaShaRK and vitaGL needed `shacccg_ext.h`/`vitashark.h` on the include path for both CC
  and CXX -> `-I$VITA_SYSROOT/include` on both.
* FFmpeg `libavutil/opt.c` failed with GCC 15 under `-fshort-enums` -> `-fno-short-enums` (and the same flag for any TU that includes FFmpeg headers).
* C++ namespace-scope `const` has internal linkage: `sceUserMainThread{Name,Priority,StackSize,CpuAffinityMask}` were absent from the first ELF
  until declared `extern const` (checked with `nm`).
* The vitasdk newlib heap is `_newlib_heap_size_user` (weak, default 128 MiB; verified by disassembling `libc_a-sbrk.o` and the sample); `sceLibcHeapSize` is not
  referenced by this newlib. GitHub access to the vitasdk repos was blocked, so the newlib sources could not be read: UNVERIFIED beyond the disassembly.
* Coroutine switch: saved SP was written to the wrong slot because `STM` stores in ascending register order; fixed (r12 carries SP) and re-validated in Unicorn.
* `vgl_hook_printf` originally truncated at 1 KiB (it would have cut `DEBUG_GLSL_TRANSLATOR` dumps): now measures the length and uses the heap.
* T6/T7/T8 counted missing-libshacccg shader failures as FAIL: now `LIMIT`, so a Vita3K run (no shader compiler) can still end in `RESULT: PASS`.
* `glGetUniformLocation` returns a negated pointer in vitaGL: tests compare against `-1`, not `>= 0`.
* The `fast` library's default scratch policy (dynamic + stream) would overwrite a persistent `GL_DYNAMIC_DRAW` ring: `GlParams::scratch_for_dynamic=false`,
  `scratch_for_stream=true` are our defaults.
* **`vita-mksfoex` defaults to `ATTRIBUTE2=12`** (checked by running it and parsing the SFO): leaving the flag out does not give a retail-memory package. The build
  now always passes `-d ATTRIBUTE2=<n>`; the `attr2-0` flavour was wrong until this was caught and rebuilt.
* Vita3K does not honour `SCE_O_TRUNC` (project roadmap: a shorter rewrite keeps the old tail): `plat::file_open(WriteTruncate)`, the log open and
  `dir_copy_tree` now `sceIoRemove` the target first.
* `log_sync` in `selftest.cfg` could never take effect (the config is read after platform init): `plat::log_set_sync()` added.

## 30 Oct 2026 - vitaGL moved from `dca4b9d` to `9bf3e3f`

* Pin in `scripts/fetch-third-party.sh`; the patches (`scripts/patches/vitagl-000{1..4}-*.patch`) applied to the new base in order.
  0001 and 0002 unchanged, 0004 only shifted (`vitaGL.h` hunk 1161 -> 1171), **0003 changed**: `vgl_gxm_state_touched()` also marks
  `dirty_shader_vert_unifs` / `dirty_shader_frag_unifs`, because upstream's `glUseProgram` no longer does when the program is unchanged and the
  clear, blit, scissor-mask and fixed-function paths reserve the default uniform buffers (reasons: `VITAGL_NOTES.md` section 14).
  **0005 dropped**: upstream `76f43e6` is the same change.
* Libraries rebuilt with the default variants (`build-sysroot.sh`, release vitaGL + FFmpeg nopt); the app with `app/build.sh --lean-lib ...`.
  Splashscreen: all variants still build with `NO_SPLASHSCREEN=1`, so upstream's new boot splash code is compiled out (checked in the object file).
* The shader cache moved: vitaGL now uses `shader_cache/v3/{v,f}/XX/<16-digit hash>.gxp` (was `v2`, unpadded). `scripts/shader-cache-to-v3.sh`
  converts the 1.0 cache (70 files, 10 of them with 15-digit names) for `build.sh --shader-cache`. The 1.0 eboot keeps using `v2`, the new one `v3`;
  both folders may exist in `ux0:data/infected/shader_cache/`.
* Upstream commits reviewed one by one against the port's GL calls: `VITAGL_NOTES.md` section 14. Expected gain is small and on the CPU side of the
  GL thread (cheaper uniform uploads, `36219c3` and `6f56e3f`); nothing here was run on the console yet.

**Console comparison (one session).** Everything except patch 0003 is compiled in, so the A side is the 1.0 `eboot.bin`:

1. Keep `eboot_1.0.bin` (the `eboot.bin` of the 1.0 VPK) and `eboot_new.bin` (the new VPK's) side by side. Install the new VPK (VitaShell); the
   install is the B side. To switch sides, copy the wanted file over `ux0:app/INFC00001/eboot.bin` (FTP or VitaShell) and start the game. Nothing else
   differs between the two VPKs but the shader cache folder it ships (`v2` vs `v3`), and `ux0:data/infected` already has the `v2` one from 1.0.
2. Same `config.txt` for every run, for example `bench=play`, `gl_timing=600` (`[gl-time]` lines: draws, uploads, swap) and no other change, so
   `[play]` fps/low1%/`guest`/`ge` and `[gl-time]` are comparable. Run each side at least twice (the bot's game differs by about 2 fps a run).
3. Runs, in this order, B first so a crash shows up early: B (new eboot); A (1.0 eboot); B with `gl_vgl_skip_redundant=0` (isolates patch 0003 in the
   new library). Optional: B with `gl_scratch_buffers=0`.
4. Before the first B run delete `ux0:data/infected/shader_cache/v3` if it exists, so the log shows the shipped files being copied
   (`shader cache: 70 shipped file(s) copied`) and no `[gl] program ... linked` slow compiles at start.
5. Look for: no vitaGL splash at start (first frames are the port's own), pictures equal to A in mission 1 (HUD, fades, post passes after clears),
   `log.txt` without `vitaGL` errors, and the same `FINAL` line fields in `[play]`.

## Open (UNVERIFIED) items, each tied to a test

See `VITAGL_NOTES.md` section 13 and `vita/README.md` section 7. Short form: `ATTRIBUTE2` honoured on 3.74 and the size of the user pool (T2), core indices
reported by `sceKernelGetCpuId` and thread priority range (T4), BGM port at 44.1 kHz (T5), shader compiler memory and runtime GLSL on hardware (T6), FBO
orientation / depth survival (T6/T7), per-draw CPU costs and the best path (T8), FFmpeg `pt` at runtime and decoder cost (T9), memory-card throughput (T10),
SceVideodec/SceAvcdec usability next to the vitaGL pools (not exercised).
