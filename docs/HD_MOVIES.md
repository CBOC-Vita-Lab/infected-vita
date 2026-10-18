# HD replacement movies (`gfx.hd_movies`)

The game's 18 pre-rendered movies (`.pmf`, 480x272, H.264 at about 1 Mbit/s, 29.97 fps) can be replaced by 960x544 versions made
offline from the same discs. The setting is **HD movies: Off / On** on the Graphics page of the in-game menu (`gfx.hd_movies` in
`settings.txt`; `INFECTED_GFX_HD_MOVIES=on|off` forces it for a run). It is greyed ("No replacement movies yet.") until
`movies_hd/index.txt` exists next to the game's data and lists at least one movie that is installed.

The replacement is only the PICTURE. The game's own movie player runs exactly as without it: it reads the PMF from the disc, fills its
ring buffer, decodes the ATRAC3plus audio, paces itself on the audio clock, skips on CROSS, stops when the menu pauses the game.

Everything here is unverified on a real PlayStation Vita; section [What was verified](#what-was-verified-and-what-was-not) says what was.

## What is replaced, and how

```
game -> sceMpegAvcDecode(access unit n, pts) ----> HLE (host/hle/hle_mpeg.cpp)
                                                    |  HdSession::show(): which picture is this? (pts -> number n)
                                                    |  source->frame_for(n)  ---- MP4 sample n, decoded (software / Vita hardware)
                                                    |  publishes the picture in HdFrameStore under (session, n)
                                                    |  writes a 32-byte MARKER into the first words of the game's buffer,
                                                    |  fills the rest black, and does NOT run the original 480x272 decoder
game copies its buffer to VRAM, draws the texture
                                                    -> renderer (host/gpu/gl): the texture it is about to sample starts with the
                                                       marker -> look up (session, n) -> sample the replacement instead
```

* **Which movie is playing.** `sceMpegQueryStreamOffset` hands the library the first 2048 bytes of the PMF (the PSMF header). The CRC-32
  of those bytes names the movie in `index.txt`. If the CRC is not listed the library falls back to the name of the last `.pmf` the game
  opened (a hook in `open_file`) when a file of that name exists and its picture count agrees. A replacement whose MP4 sample count is not
  the PSMF picture count (`(end - start) / 3003`) is refused.
* **Which picture.** The picture number is the access unit's time stamp: `n = (pts - start + 1501) / 3003`. All 18 movies are 29.97 fps
  with no B-frames, so number = decode order = display order. The same number selects MP4 sample n.
* **Nothing reads the stale buffer.** The original decode is skipped, so the guest buffer holds black plus the marker; the renderer
  resolves the marker at draw time (it only looks when a replacement was ever published). A renderer that did not know markers would show
  black, not garbage. The game's own copy, double buffering and timing decide when a picture is displayed.
* **Skip, pause, loop, end, back to back.**
  * CROSS skip: the game stops decoding and tears the movie down; the session ends with it (intro, CROSS at flip 900: 340 pictures shown, then the title
    screen pixel-identical to the original run).
  * Menu pause: the kernel pause listener pauses sessions; the Vita source needs nothing (it decodes only when asked), the desktop one
    nothing either. The picture on screen is the one the game left.
  * Seeking back (a movie restarted with `sceMpegFlushAllStream`, or the setting switched on in the middle of a movie): the source restarts at
    the sync sample at or before the wanted picture (about one second of pictures at most) and decodes forward.
  * Switched OFF in the middle of a movie: the session kept the access units since the last IDR; they are replayed into the original decoder
    first, so the next picture is correct (measured: MAD 0.63 against the source, the same as the original path).
  * Back-to-back movies (logos): one session per `sceMpegCreate`; a session ends with its player.
* **Missing or bad replacement files** never break a movie: no index entry, a missing or truncated file, an MP4 this build cannot decode
  or with the wrong picture count all end with the original movie playing, with one line in the log (`[hdmovie] <name>: <reason>; the original
  movie plays`). A decoder error in the middle of a movie shows the original picture for that access unit (and later ones, per
  the replay rule above). On the Vita the pictures that follow a failed one up to the next sync sample refer to it, so the original shows the rest of
  that group of pictures and the replacement returns at the next sync sample (`app_hd_movie.cpp`, `poison_*`); eight failures in a row end the replacement for the movie.
* **Conditions.** The replacement stands in only when: the setting is on, the game decodes into main memory as 32-bit pixels (all movies of
  this game do), the renderer supports it (`supports_hd_movies()`: the GL renderer does, the Vulkan one does not), and a replacement exists.

Code: `host/movie/{hd_movie.hpp,hd_movie.cpp,hd_frame_store.cpp,mp4_reader.*,hd_source_software.cpp}`, `host/gpu/gl/gl_hd_movie.*`, hooks in
`host/hle/hle_mpeg.cpp` (QueryStreamOffset, AvcDecode, FlushAllStream), `host/hle/hle_io.cpp` (name of the last PMF) and
`host/gpu/gl/gl_renderer.cpp` (`resolve_hd_movie`). Tests: `tests/hd_movie_tests.cpp` (MP4 reader, library, marker, store, session).

## The files

```
<data root>/movies_hd/index.txt        ux0:data/infected/ on the Vita, the folder INFECTED_DATA_DIR names on a desktop
<data root>/movies_hd/<name>.mp4       <name> is the PMF's name without extension, case as on the disc
```

`index.txt` has one line per movie: `name pictures crc32hex WxH` (comments start with `#`). `build_movies.sh` writes it from the movies it
finds, so building a few movies later keeps the others listed. A listed movie whose `.mp4` is not there is treated as not installed.

| | |
|---|---|
| Container | MP4, `moov` first or last, one video track, `avc1` + `avcC`, `stts`, `stsz`, `stsc`, `stco`/`co64`, `stss`; no audio, no composition offsets (B-frames are refused) |
| Video | H.264 **High profile, level 3.1**, 8-bit 4:2:0, **960x544** (exactly twice the PSMF's picture), **no B-frames**, 3 reference frames, closed GOP: an IDR picture at most every 30 pictures (`-g 30`, scene cuts add more) |
| Pictures | exactly the PSMF's count, 29.97 fps (`30000/1001`), time scale 1000 |
| Colour | limited range, BT.601 (the matrix the port uses for the originals), tagged `smpte170m` |
| Rate | CRF 20 capped at 2.8 Mbit/s (buffer 5.6 Mbit): 394 MB for all 18 movies |

Why these limits: they sit well inside every hardware decoder in the target list. Level 3.1 is 720p30 (a 960x544 picture is 2040 macroblocks, the
level allows 3600; 61200 macroblocks/s of 108000 allowed); High at 2.8 Mbit/s is a fraction of the 17.5 Mbit/s the level allows; 3 reference frames
and no B-frames mean no reordering, so a decoder returns one picture per access unit, in order, with no delay (the sync logic relies on that);
a closed GOP of 30 bounds the work after a seek to one second of pictures. The sources are 1 Mbit/s 480x272 video: more than 2.8 Mbit/s buys
nothing at 2x. What the **Vita's hardware decoder** accepts (Baseline, Main and High to level 4.1, up to 1280x720 pictures) comes from public
reverse-engineering notes and the Vita3K emulator, not from a device: the Vita source (`app_hd_movie.cpp`) refuses anything else.

## Making the movies: `tools/hdmovies/build_movies.sh`

```
build_movies.sh [--mode filter|ai] [options] <Infected.iso | folder with the .pmf files> <outdir>
```

It extracts the 18 PMFs from the ISO (`iso_extract.py`, ISO 9660 only; the PMFs are in `PSP_GAME/USRDIR/IMAGE/movies/` and one XMB icon),
demuxes each and compares the picture count with the PSMF header (a movie that does not match is skipped, the port would reject it too),
enlarges to 960x544, encodes, **verifies the output with ffprobe (picture count, size)**, and writes `<outdir>/movies_hd/*.mp4` and `index.txt`.
Copy the `movies_hd` folder into the data folder. It is resumable (a movie that is done is skipped; `.part` files are finished atomically), nice-d,
FFmpeg threads default to 2, nothing is written outside `<outdir>`. **Before it does anything it checks what it needs** (python 3, ffmpeg and ffprobe, the filters
`zscale`, `deblock`, `hqdn3d`, `cas`, `deband`, the encoder libx264, and for the AI mode the upscaler and its model) and prints what is missing together with the command that fixes it
(`brew install ffmpeg` on a Mac). A `movies_hd` folder holds the output of one mode only: the script refuses to put the other mode's files next to them (use another `<outdir>`).
Written for bash 3.2 and BSD tools (macOS); rebuilding `gamespy.mp4` and `pms_logo.mp4` with this version gives the same bytes as the production set (`MANIFEST.txt` in `private/hdbuild/movies_hd/`).

### `--mode filter` (default; FFmpeg only)

Chain: full-range BT.601 to limited range in 16 bit (no banding from the range change), weak deblock and denoise at the original size
(removes the 4x4 block edges and the mosquito noise of a 1 Mbit/s source, deliberately weak so film grain and snow survive), Lanczos enlargement
(zimg), `cas=0.3` restrained sharpening, 16-bit deband, error-diffusion dither to 8-bit, x264 `medium`.

**Measured here** (2 shared cores, other jobs running): all 18 movies, 51023 pictures, **5178 s (86 min)**, 394 MB. Per movie: Credits 130 MB (889 s), trivium
76 MB (813 s), agony 51 MB (659 s), chimaira 34 MB (636 s), outro 22 MB (128 s), news_report2 18 MB (403 s), tut2 18 MB (371 s), tut1 16 MB (230 s), intro 14.6 MB
(348 s), news_report9 8.6 MB, news_report6 8.9 MB, news_report11 5.2 MB, news_report12 4.5 MB, tut3 4.5 MB, pms_logo 0.8 MB, ICON1 0.46 MB (288x160: the XMB icon, never played),
majesco_logo 0.36 MB, gamespy 0.14 MB.

**How much better is it?** Honestly: *modestly*. `private/compare` in the work tree has crops of the same game frames from the original path with the
default `sharp` filter (a bicubic in the renderer), with `bilinear`, and with HD; the findings:

* Flat and dark areas: clearly cleaner. No 4x4/8x8 block edges, no mosquito noise around edges, no banding in the dark gradients (the night plaza, the
  podium wall).
* Text and logos (Majesco, Planet Moon, GameSpy, the news lower third): a little crisper than `sharp` and without its ringing; the strokes of small text
  are cleaner. About one pixel of difference at the edges, visible when zoomed 2x, hardly at normal size.
* Faces and fast action: equal to `sharp` apart from the cleanliness above. No detail is added: the source has none to add, so this is **not** "HD".
* An objective round-trip check (each of 6 clips shrunk 2x, enlarged 2x again, PSNR against the original; it measures how well a scaler rebuilds a picture that was reduced,
  which is only a proxy): Lanczos on average about 0.8 dB above bicubic and 2.5-3 dB above bilinear (intro: 35.35 / 35.08 / 33.74 dB), light sharpening another 0.1-0.2 dB. That is where `cas=0.3` comes from.

### `--mode ai` (Real-ESRGAN; for a machine with a GPU)

Replaces the enlargement with `realesrgan-ncnn-vulkan` (2x, `realesr-animevideov3` by default; `--ai-model realesrgan-x4plus` is the slower one that looks less drawn: its 4x output is scaled
down to the 2x the port wants). Per movie: pictures to PNG (full-range BT.601 to RGB by swscale) in chunks of 900 (`--ai-chunk`), Real-ESRGAN per chunk, RGB to
limited-range BT.601 with error diffusion, x264 with the same settings as filter mode, chunks concatenated and muxed. **Resumable:** a finished chunk is kept
(`<outdir>/work/<movie>_<model>_<first>_<count>_c<chunk size>/chunk_<first picture>_<count>.h264`; a different model, cut or chunk size never reuses another's chunks), and when the upscaler stops
the script says to run the same command again.

**On an Apple-silicon Mac: one command** (macOS ships bash 3.2 and BSD tools: the script was written for them). **This path was NOT run on a Mac**: the author had no Mac and no GPU; the parts that could be run
here were (see below).

```
brew install ffmpeg                                                  # libx264 and zimg are in the default build
tools/hdmovies/build_movies.sh --fetch-ai --sample 10 /path/to/Infected.iso ~/hd_out      # 10 seconds of the intro in both modes, side by side: minutes
open ~/hd_out/sample/intro_side_by_side.mp4                          # filter mode on the LEFT, AI mode on the RIGHT: look at the crowd, about 4 seconds in
tools/hdmovies/build_movies.sh --mode ai --fetch-ai /path/to/Infected.iso ~/hd_out        # the whole set; run the same line again after an interruption
tools/hdmovies/build_movies.sh --estimate --fetch-ai /path/to/Infected.iso ~/hd_out       # optional: 30 pictures, then the hours the whole set will take
```

`--fetch-ai` downloads Real-ESRGAN's own release for the system (v0.2.5.0, 47 MB Linux / 52 MB macOS from github.com/xinntao/Real-ESRGAN, **sha256 checked**; the macOS one is a universal arm64 + x86_64 binary with MoltenVK built in,
so no Rosetta and no Vulkan SDK) into `<outdir>/tools/realesrgan`, clears the macOS quarantine flag, and uses it. `--ai-bin PATH` uses one you have. Every AI run starts with the tool on one picture
(`--no-preflight` skips it): no Vulkan device, a missing model folder or a wrong output size stops it there, in a minute, not after hours; a software Vulkan device (CPU) is recognised and warned about.
**Both the start and the end of every AI run print this warning: the upscaler invents detail. On small faces (a figure 25 pixels tall) it paints mask-like faces with dark eye bars and black hair blobs;
judge the sample before the full run.**

`--only intro,tut1` limits it; `--seconds 3 --start 20` writes a short test cut to `<outdir>/test/`; `--sample S --sample-start T` is the intro sample above (default start 18 s). **Time:** 51023 pictures in all (the 12141 of the credits
alone are a quarter). This machine has no GPU, so there is no GPU measurement: on Mesa's software Vulkan (lavapipe) a picture takes 27-65 s wall on the loaded box.
There is therefore **no measurement for a Mac**: the time is unknown until `--estimate` has run there (it takes a minute); the plausible range is a few hours, and the script prints its own
extrapolation. `--only` with the tutorials, intro, logos and outro (10777 pictures) is a fifth of the work; leaving out the credits and the five news reports saves about 40 percent.

**What was run, and where** (Linux, no GPU): the whole script with `--sample 0.1 --ai-bin <lavapipe wrapper>` (3 pictures of the intro in both modes and the side-by-side file, 115 s; the picture shows the filter
and AI versions next to each other with the same colours), `--fetch-ai` (real download, checksum, unpack, preflight failing cleanly without a Vulkan device), the chunk logic with a stand-in upscaler that fails on its
second call (the first chunk is kept, the rerun continues at the second, the result has the right number of pictures), the missing-prerequisite messages, `--estimate`, `--seconds`. **Not run:** anything on macOS (BSD `sed`/`du`/`wc`
differences were avoided by reading, not by running), Apple-silicon speed, the quarantine step, the real upscaler on a real GPU.

**Quality, visually** (10 pictures of the intro, lavapipe): sharper edges and flatter textures than filter mode, and it **invents detail on small faces**: a figure 25 pixels tall in the intro got a mask-like face with dark eye bars and a black hair blob.
The mean colour is within 0.5 levels of the source in all three channels, the best-matching source picture of AI picture *i* is *i* for all 10 (no offset), MAD against the filter mode 3.4 levels.
Whether that is better is for the owner to judge on the whole set.

## Settings and switches

| | |
|---|---|
| `gfx.hd_movies` (`settings.txt`, menu: Graphics, HD movies) | `off` (default) / `on` |
| `INFECTED_GFX_HD_MOVIES=on|off` | forces the value for the run (the menu cannot change it) |
| `INFECTED_TRACE_HDMOVIE=1` | trace: library, sessions, every picture drawn, CPU per picture |
| `INFECTED_TRACE_MPEG=1` | the existing MPEG trace; an HD picture shows `(HD)` |
| Vita `config.txt` | `hd_movie_layout=packed|planar`, `hd_movie_chroma=uv|vu`, `hd_movie_csc=0|1` (escape hatches, defaults `packed`, `uv`, 0: see "What the picture format rests on"), `hd_movie_probe=1` (numbers about the layout in the log), `hd_movie_ignore_load_error=1` (emulator only) |

## Desktop: the software source

`hd_source_software.cpp` reads the MP4 itself (`mp4_reader`: the bundled FFmpeg has decoders only, no demuxer) and decodes with FFmpeg's H.264 decoder on the
guest's thread, one picture per access unit. The picture is converted to RGBA on the render thread (Q8 BT.601, limited range; measured mean RGB within 0.5 levels
of the original path) and uploaded into one GL texture. CPU cost, measured (1500 flips = 1283 movie pictures plus the boot, lean code, llvmpipe, 2 shared cores, process user+sys):
original path with `gfx.movie_filter=sharp` 43-45 s, original with `bilinear` 15.1 s, **HD 17.1-17.9 s**. Per HD picture: decode 3.8-4.1 ms (guest thread), conversion 1.9-2.0 ms
and upload call 0.23 ms (render thread): about +2 ms over the cheapest original path; the original 480x272 decode is skipped.

## The Vita: hardware decoder, in lockstep

`vita/app/src/app_hd_movie.{h,cpp}`. The source is registered with the framework (`set_hd_source_factory`) and the renderer's binder (`set_hd_native_binder`); nothing is
loaded until a replacement movie is opened. It uses **SceVideodec/SceAvcdec** (`sceAvcdecDecode`), not `sceAvPlayer`, on purpose:

* **Frame accurate by construction.** `sceAvPlayer` runs on its own wall clock; matching it to the game's access-unit time stamps needs `sceAvPlayerJumpToTime`
  and drift correction, and the Vita3K emulator has neither (`JumpToTime` is a stub). With the decoder in lockstep the picture of unit *n* is decoded when the game
  decodes unit *n*: there is nothing to drift, pause costs nothing, skipping and seeking use the same sync-sample logic as the desktop. The offset is zero by construction;
  there is no clock whose drift could be measured.
* **No copy and no CPU conversion.** The decoder writes 4:2:0 into one of 12 slots of physically contiguous memory (9 MiB, plus the decoder's own memory and a 1 MiB stream buffer; the
  26 MiB of phycont are otherwise unused because vitaGL gets none: `GlParams::phycont_pool_mib = 0`). Each slot has its own GL texture whose GXM descriptor was re-pointed at it
  (`sceGxmTextureInitLinear`, YUV420 format, the technique of vitaGL's `video_playback` sample); the GPU's texture unit converts YUV to RGB while the renderer draws the game's
  textured quad. The renderer needs no new shader (a new shader would need the shader compiler on the device). 12 slots = the framework's store (8) + the renderer (1) + the
  source (1) + one being decoded, oldest-released-first so the GPU has finished with a slot long before it is reused; if all are held the original picture is shown.
* Checks at open (any failure = the original movie plays, with the reason in the log): 960x544, Baseline/Main/High, level <= 4.1, sync samples at most 600 apart, no sample larger
  than the 1 MiB stream buffer, decoder free (one at a time).
* Each access unit goes in with its sample number as the time stamp; the picture the decoder returns must carry the same number, or the picture is rejected.
* **When a picture cannot be decoded** the pictures after it in the same group (up to the next sync sample) refer to the broken one, so the decoder is not fed them: the original shows the rest of the group
  (the log says `picture N could not be decoded; the original shows pictures N to M`) and the replacement comes back at the next sync sample. After 8 groups in a row that fail the movie falls back to the original for good.
  The same fall-back, with the original decoder given what it has not seen (and the SPS/PPS, see below), covers a file that ends early, the setting switched off in the menu and all 12 frame slots being held.

### What the picture format rests on, and where each fact comes from

The defaults are what the SDK's own definitions say; each fact has its source, and the three `config.txt` keys are escape hatches for a console that disagrees (`HD_MOVIES_HARDWARE_TEST.md` says when to use which).

1. **Decoder output layout.** vitasdk `psp2/videodec.h`: `SCE_AVCDEC_PIXELFORMAT_YUV420_PACKED_RASTER` (0x20) is the luma plane followed by one plane of interleaved chroma, `YUV420_RASTER` (0x10) three planes. Vita3K
   (`SceVideodecUser.cpp`, `h264.cpp copy_yuv_data_from_frame`) writes the packed one as Y then U V U V ... (NV12) and the planar one as Y, U, V (I420). Default: **packed, U first**. (`hd_movie_layout`, `hd_movie_chroma`)
2. **How GXM names it.** A GXM texture format has a base (`YUV420P2`: luma + one chroma plane; `YUV420P3`: three planes), a chroma order and a colour-conversion slot (`CSC0`/`CSC1`). For three planes the names read as they say (I420 = `YUV420P3`);
   for two planes **SCE's names are the other way round: NV12 (U first) is `YVU420P2`**. Sources: SDL's Vita renderer (`SDL_render_vita_gxm.c`: NV12 to `YVU420P2_CSC0`, IYUV to `YUV420P3_CSC0`, with the comment "should be the other way around. looks like SCE bug");
   vitaGL (`source/textures.c`: `VGL_YUV420P_NV12_BT601` to `YVU420P2_CSC0`, `VGL_YVU420P_NV21_BT601` to `YUV420P2_CSC0`, `VGL_YUV420P_BT601` to `YUV420P3_CSC0`); vitaGL's `samples/video_playback` (sceAvPlayer frames, NV12, bound as `YVU420P2`). Default: **`YVU420P2_CSC0`**.
3. **Which matrix.** vitaGL maps its `*_BT601` formats to CSC0 and `*_BT709` to CSC1; SDL programs the CSC0 slot with `sceGxmSetYuvProfile(ctx, 0, SCE_GXM_YUV_PROFILE_BT601_STANDARD)` for limited-range BT.601 content (vitasdk `gxm.h` lists
   BT601/BT709 x STANDARD/FULL_RANGE); vitaGL never calls `sceGxmSetYuvProfile`, so what its BT601 formats get is the context's default for slot 0. The replacement movies are limited-range BT.601, so the default is **CSC0** (the first version of this code used CSC1,
   BT.709, which was wrong). (`hd_movie_csc`)
4. **The descriptor's mip count field must stay 15** ("no mip chain": what `sceGxmTextureInitLinear(.., 0)` writes, as in vitaGL's `video_playback`). vitaGL's `glTexParameteri(GL_TEXTURE_MIN_FILTER)` on a `GL_TEXTURE_2D` rewrites it to 0
   (`textures.c`: `vglSetTexMipmapCount(.., use_mips ? mip_count : 1)`), and the texture unit places the chroma plane right behind the luma plane only for 15; with 0 Vita3K looks for it behind a power-of-two sized luma area
   (`renderer/src/texture/cache.cpp`, `layout_width`/`layout_height`): that was the green and magenta picture of the first emulator run, whose chroma came out of the neighbouring frame slots. The renderer therefore never calls `glTexParameteri` on these textures
   (`resolve_hd_movie` seeds its sampler cache) and the binder writes the descriptor again every time it is used.
5. **The decoder module.** `sceSysmoduleLoadModule(SCE_SYSMODULE_AVCDEC)` fails in Vita3K (0xC05A1000: its sysmodule emulation looks for a module file the firmware lacks; the decoder itself is built into the emulator). On a console it loads. A failed load is
   final for the process and the original movies play; `hd_movie_ignore_load_error=1` goes on regardless and is **for the emulator only**.

Still unverified on a console: that the decoder's chroma byte order and the default profile of CSC0 are what the sources above say. `hd_movie_probe=1` makes the log carry numbers about the layout (means of the even/odd bytes and of the halves of the chroma
area, the descriptor words) so that a wrong guess can be diagnosed from the log alone.

### Memory: what is resident, and when

| What | Where | Size | While |
| --- | --- | --- | --- |
| hardware decoder (`sceAvcdecQueryDecoderMemSize`) | phycont (`USER_MAIN_PHYCONT_NC_RW`) | 3072 KiB (Vita3K's figure; the log prints what the console says) | from the first replacement movie until 120 frames after the last replacement picture |
| stream buffer | phycont | 1024 KiB (the biggest sample of the production movies is far below) | same |
| 12 frame slots, mapped for the GPU (`sceGxmMapMemory`) | phycont | 12 x 768 KiB = 9216 KiB (a 960x544 4:2:0 frame is 765 KiB) | same |
| 12 GL texture objects, one per slot | vitaGL (descriptors only: the 8x8 placeholder storage is freed at once) | about nothing | same |
| the movie's sample table, one sample buffer, the replay history of the framework | heap | about 32 bytes a picture (intro 64 KB, credits 390 KB) + the largest sample + at most one group of pictures | while the movie plays |

**13312 KiB of phycont** (Vita3K's pool is 26 MiB; the console's size is in the log line), nothing in CDRAM and nothing in the user heap that scales with the picture: the frames are sampled where the decoder wrote them. **Coexistence with the HD textures:** the texture packs live in CDRAM
(the vitaGL pool; budget 32 MB for the 2x pack, 48 MB for the mixed 4x pack) and the movie frames in phycont, a pool vitaGL does not use (`GlParams::phycont_pool_mib = 0`), so a movie neither takes budget from the textures nor evicts any of them, and the textures do not
shrink the room for the decoder. The two meet only in GPU time (the YUV sampling of one full-screen quad) and CPU time (the decode is in hardware; the framework's marker and bookkeeping are microseconds).

**Release when no movie plays.** The renderer counts frames since a draw last used a replacement picture; after 120 (about 2 seconds at 60 fps: a fade after the movie still finds its last picture) with no movie session left it drops its picture and texture and the
frame store's pictures, then calls the binder's `release_frames()`: the Vita deletes the 12 GL textures (render thread, GL context current), frees the three phycont blocks, terminates the decoder library and unloads the module. If a picture is
still alive somewhere, or a movie is being set up at that moment, it is asked again at the next frame; nothing is freed that the GPU may still read. The next movie sets it all up again (module load, three allocations: tens of milliseconds: 19 ms from the first to the last of the lines below in Vita3K).
The log shows both ends: `hdmovie: memory before the first picture, decoder not set up: phycont_free=... cdram_free=... user_free=...`, `hdmovie: memory with the decoder set up: ...`, `hdmovie: memory after the last movie, decoder given back: ...`, each with the number of decoder blocks that exist
(0 after the release). **On a console `phycont_free` after must equal `phycont_free` before.** Vita3K cannot show this (its `ksceKernelFreeMemBlock` subtracts `sizeof(SceKernelMemBlockInfo)` instead of the block's size, so its free-memory figure never goes back up): there the block count
and the log line `[hdmovie] frame N: the replacement pictures are released` are the proof.

## What was verified, and what was not

Desktop (lean code, virtual clock, llvmpipe, scripted pad flow), all with the 18 movies of `build_movies.sh`:

* **Identification and replacement:** logos (majesco, pms, gamespy), intro and tut1 play unskipped with HD on: every access unit replaced, 0 original pictures, 0 seeks (intro 2010 of 2010, tut1 2091 of 2091).
* **Sync with the original path** (same flips of two runs; the picture shown at a flip is found by matching the screenshot against the original movie: `match_frames.py`): intro, 40 consecutive
  flips: identical at 40 of 40; tut1, 40 consecutive flips: identical at 39 of 40 (one flip shows the neighbouring picture; the original path differs from a second run of itself at about the same rate, it is
  run-to-run noise of the threaded renderer, not an offset); tut1, 11 flips spread over the whole movie: identical at 8 of 11, two undecidable (still scenes), one off by one. The AU time-stamp sequence the game passes
  is identical in both runs, so there is no drift to accumulate: the picture is a function of the game's time stamp only.
* **CROSS skip** (intro, flip 900): the session ends after 340 pictures, the following flips are pixel-identical to the original run. **Menu during a movie:** pause and resume
  (44 consecutive flips around two pauses: identical at 42; two differ by one picture right after the resume, as the original does against itself). **Toggle during a movie:** on to off to on: clean
  original picture after switching off, one seek on switching on, 1809 HD + 201 original pictures.
* **Damaged or missing replacements** (software source, intro unless said otherwise; `INFECTED_TRACE_HDMOVIE=1` shows each case in the log): random bytes in one sample, or an AVCC length prefix of `0xFFFFFFF0`: that
  picture is shown by the original, 2009 HD + 1 original, the movie goes on in HD; the file cut at 40 %: HD up to picture 821, the original from 822 to the end of the movie (1188 pictures); a file that is missing, a
  picture count that does not match the movie, a truncated header: the original plays the whole movie, one log line says why. In each case the session ends, the decoder and frames are released after the idle period,
  nothing crashes or hangs. The cut file found a bug: the original decoder was replayed from an IDR unit that carries no SPS/PPS (one IDR unit in six of the discs' movies has none) and failed 46 pictures in a row
  ("non-existing SPS/PPS 0 referenced"); the framework now remembers the parameter sets and puts them in front of the replayed units (0 decoder errors, same run).
* Non-threaded renderer: pixel-identical to the threaded one.

Not verified: anything on a real Vita (hardware decoder behaviour and byte layout, GXM colour matrix, memory block alignment, speed, the settings row on the device); Vita3K is an emulator (see the next section for what it showed).
Not tried: the Vulkan renderer (does not support replacements), PSP-style 16-bit movie modes (the game does not use them), a movie that loops.

## Vita3K (emulated Vita) result

How it was run: the real Vita app (`dist/infected.vpk`, only the title id changed so that nothing else in the shared emulator is touched) in headless Vita3K, software Vulkan (lavapipe), `trace_hdmovie=1`, `hd_movie_ignore_load_error=1`.
**Pictures per second and "longest wait" in these runs say nothing about a console** (the emulator decodes in software and runs on whatever else the machine is doing: 6 to 40 pictures/s were seen). Scripted input: a pad script (`pad_script_file`) for the
CROSS skips, Vita3K's keyboard for the menu. Screenshots are compared with `tools/hdmovies/compare_shot.py`.

* **Colour** (the hardware-decoder path, hardware texture sampling of NV12): before the fix the picture was green and magenta (mean absolute difference against the MP4's own decode, in levels of 255: R 47.6, G 75.3, B 35.8; signed R -44.5, G +74.4, B -30.5).
  After: intro pictures 160, 281, 402, 522, 640, 760 and, in later runs, 483, 592, 667, 922, 1128, 1353, 1522, 1756: **R 0.30 to 0.41, G 0.92 to 1.26, B 0.27 to 0.40 levels** (signed G about -1.0, R about +0.2, B about +0.35: Vita3K's own YUV conversion rounds that way). The desktop
  shows the same pictures within 0.2 levels in every channel, the difference between the two is Vita3K's conversion, not the movies'. The mip-count field of the descriptor read 15 in every slot (`hd_movie_probe=1`). On a console the matrix of CSC0 is the remaining unknown.
* **Sessions:** majesco_logo 184, pms_logo 185, gamespy 180, intro 2010 pictures, all replacement pictures, 0 original, 0 seeks; one decoder set-up for all four (back to back).
* **Memory** (the lines `hdmovie: memory ...` of the log): before the first picture `phycont_free=26624 KiB cdram_free=0 KiB user_free=101376 KiB`, with the decoder set up `phycont_free=13312 KiB` and 3 decoder blocks (decoder 3072 KiB + stream 1024 KiB + 12 frames of 768 KiB = 13312 KiB), after the last
  movie 0 decoder blocks, the textures deleted and `[hdmovie] frame N: the replacement pictures are released` 120 frames after the last movie session ended (a session that gave up still counts until its movie ends; `phycont_free` stays at 13312 in the emulator: its free-memory figure does not rise on free, see above; the block count is the proof).
  `cdram_free` reads 0 and `user_free` stays at 101376 KiB in every Vita3K log, before, during and after (what the emulator's `sceKernelGetFreeMemorySize` reports does not move): neither is a measurement here. On a console the three figures are the kernel's own.
* **CROSS skip** (logos and intro): the sessions end after 102, 76 and 771 pictures with 0 original, the next movie starts, the decoder is released after the last one.
* **Menu during a movie** (menu open about 8 s at picture 483): the movie goes on at picture 592 after the close, 2010 of 2010 replacement pictures, no jump back.
* **Setting switched Off in the menu during the intro** (picture 417): the original picture shows at once (not black), 417 HD + 1593 original, no error. **Switched On in the menu during the intro** (picture 734, started with Off): 734 original, then HD to the end, 1 seek, the decoder is set up at that moment.
  **Off and back On in the same movie** (pictures about 330 and 600): 1728 HD + 282 original, 1 seek; the screenshots after the switch to Off differ from the HD picture by 1.6 to 2.1 levels (the original path), after the switch back to On by 0.3 to 1.0 (the HD picture again).
* **Replacements that cannot be played** (the Vita source, damaged on purpose; the original shows every picture it must, the movie goes on, nothing hangs, the decoder is released afterwards):
  one sample of the logo replaced by random bytes: `picture 100 could not be decoded; the original shows pictures 100 to 112`, HD again from 113 (171 HD + 13 original); a file cut at 45 %: HD to 89, the original from there, retries at the following sync samples fail the same way (90 HD + 95 original);
  an intro cut at 10 %: HD to 175, 8 failing groups in a row, then `the original movie plays from here on` (176 HD + 1834 original); a picture count that does not match, or a missing file: the original plays and one line says why (or nothing is said when the library does not list the file at all).

Not shown by the emulator: the console's decoder itself (the emulator's is software and tolerant), the colour matrix of the console's texture unit, the real memory figures, the speed.
