// platform/platform.h - the platform layer contract for the Infected port (ARCHITECTURE.md section 6).
//
// Two implementations with the SAME interface:
//   platform/vita/             PS Vita (vitasdk, vitaGL, sceAudioOut, sceCtrl ...)           -> libinfected_platform_vita.a
//   platform/desktop_headless/ Linux x86-64 / macOS, no window, no GL (stubbed), null audio   -> libinfected_platform_desktop.a
//
// Rules
//   * C++20, builds with -fno-exceptions -fno-rtti. No std::thread/std::mutex in the Vita build (not guaranteed by the
//     vitasdk libstdc++); use plat::Thread / plat::Mutex / plat::Event below.
//   * Every function is callable from any thread unless it says otherwise.
//   * "core" always means a Vita user core 0..2 (core 3 is reserved by the system). Core roles in the architecture:
//         0 guest executor   1 GE/GL thread (vitaGL)   2 audio pump + decoders
//     On the desktop twin the core number is mapped onto host CPUs modulo the CPU count (best effort, never an error).
//   * Nothing here allocates on the hot path; everything that allocates says so.
//   * No game data is read or written by this layer. The only paths it knows are the data root (writable) and the app
//     root (read-only, inside the VPK).
//
// UNVERIFIED markers: anything not verified against headers/Vita3K source/hardware is tagged UNVERIFIED in the comment.
#pragma once

#include <cstddef>
#include <cstdint>

#if defined(__GNUC__)
#define PLAT_PRINTF(fmt_idx, first_arg) __attribute__((format(printf, fmt_idx, first_arg)))
#else
#define PLAT_PRINTF(fmt_idx, first_arg)
#endif

namespace plat {

// ============================================================================================ identity / lifecycle
enum class Kind { Vita, DesktopHeadless };
Kind kind();
const char* name();  // "vita" or "desktop_headless"

struct InitParams {
    // Directory name below the data root where the log/cfg/caches live. The data root itself is fixed:
    //   Vita:    "ux0:data/infected/"     (created with mkdir -p semantics)
    //   Desktop: $INFECTED_DATA_ROOT, else "./infected_data/"
    // Exactly one process may use a data root at a time (the log is opened for truncate).
    const char* log_file = "log.txt";      // relative to data root; nullptr = no file log
    bool log_flush_each_line = true;       // true: every plat::logf line reaches the filesystem layer before logf returns
    bool log_sync_each_line = false;       // true: additionally sceIoSyncByFd per line (slow; for crash hunting only)
    bool set_max_clocks = true;            // Vita: scePowerSet*ClockFrequency(444/222/222/166), results logged
    bool init_pad = true;                  // Vita: sceCtrlSetSamplingMode(ANALOG)
};

// Creates the data root, opens the log, optionally raises the clocks. Safe to call once. Returns false only if the data
// root cannot be created (the log then goes to the debug console only).
bool init(const InitParams& p = InitParams());
void shutdown();                           // closes the log; stops the audio pump if still running
[[noreturn]] void exit_process(int code);  // Vita: sceKernelExitProcess; desktop: _exit

// ============================================================================================ logging
// Lines are "[ssss.mmm c] text" (seconds since init, core id). The Vita build also mirrors every line to sceClibPrintf
// (visible in Vita3K's log and on a UART/PrincessLog). Thread safe. Lines longer than 1000 bytes are truncated.
void logf(const char* fmt, ...) PLAT_PRINTF(1, 2);
void log_raw(const char* text, size_t len);   // no prefix, no newline added (used by the vitaGL printf hook)
void log_flush();                              // push everything to the filesystem layer now (Vita: sceIoSyncByFd)
void log_set_sync(bool on);                    // same as InitParams::log_sync_each_line, but switchable after init (e.g. from a config file)
const char* log_path();                        // full path of the log file ("" if none)

// ============================================================================================ clock
// Monotonic microseconds since process start. Vita: sceKernelGetProcessTimeWide (64 bit, no wrap). Never goes backwards.
uint64_t now_us();
void sleep_us(uint32_t us);                    // Vita: sceKernelDelayThread; granularity ~ 1 ms is NOT guaranteed (T4 measures it)
void yield();

struct ClockReport {
    int req_arm = 0, req_bus = 0, req_gpu = 0, req_xbar = 0;   // MHz requested
    int arm = 0, bus = 0, gpu = 0, xbar = 0;                   // MHz read back (0 when the platform has no such notion)
    int rc_arm = 0, rc_bus = 0, rc_gpu = 0, rc_xbar = 0;       // return codes of the setters (0 = ok)
};
// Vita: scePowerSetArmClockFrequency(444), BusClock(222), GpuClock(222), GpuXbarClock(166), then reads all four back.
// Order matters on hardware (GPU/XBAR after ARM/BUS is what homebrew does; UNVERIFIED that order is significant).
// Logs one line with the result. Desktop: reports zeros. Safe to call repeatedly.
ClockReport clocks_set_max();
// Sets the ARM clock alone (perf.cpu_clock: 444 or 500 MHz). Vita: scePowerSetArmClockFrequency(mhz) and a read back; the
// retail firmware accepts up to 444, anything above needs a plugin that lifts the limit (the call then returns an error, which
// is logged and returned). Returns the setter's return code (0 = ok), `applied_mhz` (may be null) the clock read back.
// Desktop: returns -1 and applied_mhz 0.
int clock_set_arm(int mhz, int* applied_mhz);
ClockReport clocks_read();                     // read only
// The ARM clock as it really runs, in MHz, measured with a loop of dependent decrements (one cycle an iteration on the Cortex-A9;
// the best of three runs, about 12 ms in all). A PSVshellPlus profile runs the CPU at 500 MHz while scePowerGetArmClockFrequency
// still answers 444, so this is the number to trust. Desktop: 0.
int clock_measure_mhz();

// ============================================================================================ memory
struct MemInfo {
    int64_t user_free = -1;                    // Vita: sceKernelGetFreeMemorySize().size_user    (USER_RW pool, the newlib heap lives here)
    int64_t cdram_free = -1;                   //                                  .size_cdram   (USER_CDRAM_RW, "VRAM", GPU only in practice)
    int64_t phycont_free = -1;                 //                                  .size_phycont (USER_MAIN_PHYCONT_*_RW)
    int64_t malloc_arena = -1;                 // newlib mallinfo().arena   (bytes the heap has taken from its block so far)
    int64_t malloc_used = -1;                  // newlib mallinfo().uordblks
    int64_t malloc_free = -1;                  // newlib mallinfo().fordblks
};
bool mem_info(MemInfo* out);

struct MemBlock {
    void* base = nullptr;                      // aligned as requested
    size_t size = 0;                           // usable bytes starting at base
    int uid = -1;                              // platform handle (Vita: SceUID of the memblock)
    void* raw_base = nullptr;                  // what the OS returned (equal to base unless extra alignment slack was needed)
    size_t raw_size = 0;
};
// Allocates one big, zero-filled, cached, read/write block with the requested alignment (power of two, e.g. 1 MiB).
//   Vita:    sceKernelAllocMemBlock("name", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW /*0x0C20D060*/, roundup(size, 4 KiB)), then
//            sceKernelGetMemBlockBase. If the base is not aligned, the block is re-allocated with `align` bytes of slack.
//            Why USER_RW: it is the normal cached user pool (the same one the newlib heap and thread stacks use), so the
//            ARM can run at full speed on it; USER_RW_UNCACHE would make every guest load/store uncached (>10x slower),
//            CDRAM/PHYCONT are GPU-oriented, small and not meant for CPU heaps. Size is subject to the user pool limit:
//            256 MiB minus the newlib heap minus modules/stacks, plus 29/77/109 MiB with extended memory mode
//            (SFO ATTRIBUTE2 bits 2..3 = 1/2/3 -> ATTRIBUTE2 = 4/8/12; see vita/README.md section "Memory").
//   Desktop: mmap(MAP_ANONYMOUS|MAP_PRIVATE), aligned.
// Memory is NOT touched; call touch_pages() if you want it resident. Returns false on failure (logged).
bool mem_alloc_block(const char* name, size_t size, size_t align, MemBlock* out);
void mem_free_block(MemBlock* b);
// Writes one byte per 4 KiB page (value 0 on fresh blocks so contents stay zero) to force residency; returns microseconds.
uint64_t mem_touch_pages(void* base, size_t size);

// ============================================================================================ threads
// A thread is created and started in one call. The entry function must return normally (no exit_thread).
using ThreadFn = void (*)(void* arg);
struct ThreadDesc {
    const char* name = "plat-thread";
    ThreadFn fn = nullptr;
    void* arg = nullptr;
    size_t stack_bytes = 256 * 1024;           // Vita: allocated from the user pool, 4 KiB aligned; main-thread-like depth needs more
    int core = -1;                             // 0..2 = pin to that core, 3 = the system's core (only when core3_usable(), else core 2), -1 = any user core
    int priority = 0;                          // relative: 0 = default, negative = more urgent. Vita: 0x10000100 + priority,
                                               // clamped to [-32, +31] (matches Vita3K; UNVERIFIED that real HW accepts exactly this range)
};
struct Thread;                                 // opaque
Thread* thread_start(const ThreadDesc& d);     // nullptr on failure (logged)
void thread_join(Thread* t);                   // waits for the entry function to return, then destroys the thread object
int thread_current_id();                      // a number that is different for every live thread (Vita: sceKernelGetThreadId())
int thread_current_core();                     // Vita: sceKernelGetCpuId() (UNVERIFIED which index it reports for core 0..2: 0..2 expected)
bool thread_set_core(Thread* t_or_null_for_self, int core);    // Vita: sceKernelChangeThreadCpuAffinityMask(thid, 0x10000 << core)
bool thread_set_priority(Thread* t_or_null_for_self, int priority);  // same relative scale as ThreadDesc::priority

// The Vita has four cores and an application may run on three of them: the fourth belongs to the system (about a tenth of it is
// in use) unless a kernel plugin lifts the restriction (GrapheneCt's CapUnlocker, which allows the CPU 3 affinity mask). core3_usable()
// finds out once by starting a thread with that mask and asking where it ran. helper_core() is where the threads that only
// help go (the audio pump, the movie decoder, the GC thread of vitaGL, the watchdog): core 3 when it can be used, else core 2,
// where they share the display-list walker's core as before. core3_allow(false) (config.txt core3=0) keeps everything off core 3.
// Desktop: core3_usable() is false and helper_core() is 2.
void core3_allow(bool allowed);
// Whether core3_usable() may load the package's own kernel module (vita/core3) when the system refuses the CPU 3 mask; config.txt
// core3_unlock=0 turns that off (core 3 is then used only where a plugin such as CapUnlocker already allows it).
void core3_unlock_allow(bool allowed);
bool core3_usable();
int helper_core();

struct Mutex;                                  // non-recursive mutex; Vita: SceKernelLwMutex
Mutex* mutex_create(const char* name);
void mutex_lock(Mutex*);
void mutex_unlock(Mutex*);
void mutex_destroy(Mutex*);

// Host event: the primitive the idle/vblank logic of the guest kernel sleeps on.
//   auto_reset = true : one waiter wakes per signal and the event clears (like a binary semaphore).
//   auto_reset = false: stays signalled until event_reset().
// Vita: SceKernelEventFlag, attr SCE_EVENT_WAITMULTIPLE (0x1000). Waiting uses SCE_EVENT_WAITOR (+ WAITCLEAR for auto reset).
struct Event;
Event* event_create(bool auto_reset, bool initially_signalled = false);
void event_signal(Event*);
void event_reset(Event*);
// timeout_us < 0: wait forever; 0: poll. Returns true if the event was signalled, false on timeout.
bool event_wait(Event*, int64_t timeout_us);
void event_destroy(Event*);

// ============================================================================================ files
// All relative paths are resolved against a root. Data = the writable data root; App = the read-only package contents
// (Vita "app0:", desktop: $INFECTED_APP_ROOT or "./app/"). Paths use '/', no leading slash, no "..".
enum class Root { Data, App };
// WriteTruncate: on Vita the existing file is removed first and then created, because Vita3K does not honour SCE_O_TRUNC (rewriting a
// longer file with a shorter content keeps the old tail there). Append creates the file when it is missing.
enum class FileMode { Read, WriteTruncate, Append };

// Resolves to a full path in out (NUL terminated). Returns false if it does not fit.
bool path_resolve(Root r, const char* rel, char* out, size_t out_size);
bool dir_make(Root r, const char* rel);                       // mkdir -p
bool file_exists(Root r, const char* rel);
bool file_remove(Root r, const char* rel);
int64_t file_size_of(Root r, const char* rel);                // -1 if missing

// Buffering advice (selftest T10 measures read/write throughput versus chunk size and prints it to selftest.txt; the rule of thumb below is
// the design assumption that run is meant to confirm, UNVERIFIED until a Vita run):
//   * Vita sceIo calls are syscalls with a fixed cost per call: use reads of >= 64 KiB (256 KiB..1 MiB is the sweet spot
//     for streaming) into a 64-byte aligned buffer; do not issue 4 KiB or smaller reads on a hot path.
//   * file_open(..., buffer_bytes) adds a read-ahead buffer (Read mode) or a write-combining buffer (Write/Append) for code
//     that does many small sequential accesses. Random access should use buffer_bytes = 0.
//   * Reads from the memory card (ux0:) are not cached by the OS in a way you can rely on; keep your own cache.
struct File;
File* file_open(Root r, const char* rel, FileMode m, size_t buffer_bytes = 0);   // nullptr on failure
int64_t file_size(File*);
int64_t file_tell(File*);
bool file_seek(File*, int64_t offset);                         // absolute
size_t file_read(File*, void* dst, size_t n);                  // returns bytes read (short at EOF)
size_t file_write(File*, const void* src, size_t n);           // returns bytes written
bool file_flush(File*);
void file_close(File*);                                        // flushes
// Whole-file helpers; the buffer comes from malloc and must be released with free().
bool file_read_all(Root r, const char* rel, void** data, size_t* size);
bool file_write_all(Root r, const char* rel, const void* data, size_t size);
// Recursively copies a directory tree App:src_rel -> Data:dst_rel. Existing destination files are kept unless overwrite.
// Used to install the pre-shipped shader cache out of the VPK. Returns number of files copied, -1 on error.
int dir_copy_tree(const char* src_rel_app, const char* dst_rel_data, bool overwrite);

// ============================================================================================ pad
// Buttons use the PSP bit layout so the guest sees exactly what sceCtrl returns on a PSP.
enum PadButton : uint32_t {
    PAD_SELECT = 0x00000001, PAD_START = 0x00000008,
    PAD_UP = 0x00000010, PAD_RIGHT = 0x00000020, PAD_DOWN = 0x00000040, PAD_LEFT = 0x00000080,
    PAD_LTRIGGER = 0x00000100, PAD_RTRIGGER = 0x00000200,
    PAD_TRIANGLE = 0x00001000, PAD_CIRCLE = 0x00002000, PAD_CROSS = 0x00004000, PAD_SQUARE = 0x00008000,
    PAD_HOME = 0x00010000,
};
struct PadState {
    uint32_t buttons = 0;                      // PadButton bits
    uint8_t lx = 128, ly = 128;                // PSP analog stick: 0..255, 128 = centre, y grows downward (same as Vita left stick)
    uint8_t rx = 128, ry = 128;                // Vita right stick, not visible to a PSP game (kept for debug shortcuts)
    uint32_t raw_buttons = 0;                  // untouched Vita SceCtrlData::buttons
    uint64_t time_us = 0;                      // now_us() at poll time
};
// Vita mapping (sceCtrlPeekBufferPositive2(0, &pad, 1)): SELECT START D-pad TRIANGLE CIRCLE CROSS SQUARE map 1:1 (the bit
// values are identical on Vita and PSP); the handheld's shoulder buttons arrive as SCE_CTRL_LTRIGGER/RTRIGGER (same bits as
// the PSP) and DS4/PSTV L1/R1 are OR-ed into PAD_LTRIGGER/PAD_RTRIGGER; L3/R3, volume, headphone etc. are dropped; the PS
// button (SCE_CTRL_PSBUTTON, only visible when not intercepted) maps to PAD_HOME. Left stick -> lx/ly with a small dead
// zone of 8 around 128 so a worn stick does not drift the PSP analog (the PSP game applies its own dead zone as well).
// Returns false if the pad could not be read (state is then neutral).
bool pad_poll(PadState* out);
// The front touch screen as a button for the settings menu: true once per finger-down inside the top-right corner (about the
// right 16 % of the width and the top 20 % of the height of the 1920 x 1088 panel). Edge triggered: a finger that stays down
// does not repeat. Call from the thread that polls the pad. Desktop: always false.
bool touch_corner_tap();

// ============================================================================================ audio out
// One stereo s16 stream at `sample_rate` (44100 for the PSP game). A pump thread (core 2 by default) pulls fixed-size
// periods from a lock-free single-producer/single-consumer ring and hands them to the hardware; the producer (guest audio
// thread or decoder) calls audio_write(). The producer never blocks and never locks.
struct AudioParams {
    int sample_rate = 44100;
    int ring_frames = 16384;                   // power of two; 16384 frames = 371 ms at 44.1 kHz
    int period_frames = 1024;                  // frames per sceAudioOutOutput; Vita rounds the port grain to a multiple of 64
    enum Port : int { Auto, Main48k, Bgm44k } port = Auto;   // fixed underlying type: layout is independent of -fshort-enums
    int core = -1;                             // -1: helper_core() (core 3 when it can be used, else core 2)
    int priority = -16;                        // pump thread priority, relative (more urgent than the producer)
};
// Auto = Main48k (the MAIN port only accepts 48000 Hz, so the pump resamples 44.1 -> 48 kHz with a 4-point cubic
// interpolator (audio_resample.h); the same happens in Vita3K which enforces 48000 for MAIN) unless sample_rate == 48000.
// Bgm44k opens SCE_AUDIO_OUT_PORT_TYPE_BGM at the native rate (no resampling) - UNVERIFIED that BGM accepts 44100 on
// real hardware at FW 3.74 and that it is not ducked/stopped by the system's background-music setting; the selftest
// (T5) tries both and logs what each returned.
bool audio_open(const AudioParams& p);
void audio_close();
bool audio_is_open();
// Producer side. Copies up to `frames` stereo frames (interleaved L,R int16) into the ring; returns how many were
// accepted (< frames when the ring is full). Wait-free.
size_t audio_write(const int16_t* interleaved_stereo, size_t frames);
size_t audio_free_frames();                    // space available to the producer
size_t audio_queued_frames();                  // frames waiting for the pump
void audio_set_volume(float v);                // 0..1 software gain applied in the pump (default 1)
// Pull mode. With a source installed (before audio_open) the pump takes its frames from fn(out, frames) on its own thread, at the
// speed of the hardware, instead of from the ring: fn must fill all `frames` stereo frames (silence where nothing was written) and
// return quickly. audio_write() is not used then. Returns false on a platform that only pumps from its ring (desktop_headless).
// `fill` (optional) says how many frames the producers have queued ahead of the device, or -1 when none has written lately; the pump
// follows it with a slow, small change of its resampling ratio (audio_rate.h) so that the device's clock and the game's stay together.
using AudioSource = size_t (*)(int16_t* out, size_t frames);
using AudioFill = int64_t (*)();
bool audio_set_source(AudioSource fn, AudioFill fill = nullptr);
struct AudioStats {
    uint64_t periods_out = 0;                  // sceAudioOutOutput calls completed
    uint64_t frames_in = 0;                    // frames accepted from the producer
    uint64_t frames_out = 0;                   // frames consumed from the ring (source rate)
    uint64_t underruns = 0;                    // periods that had to be (partly) filled with silence
    uint64_t underrun_frames = 0;
    uint64_t overruns = 0;                     // audio_write calls that could not store everything
    uint64_t output_errors = 0;                // negative returns from sceAudioOutOutput
    uint32_t max_output_us = 0;                // longest sceAudioOutOutput call
    int32_t rate_ppm = 0;                      // pull mode: the resampling ratio's correction by the clock follower (+: consuming faster)
    int port_rate = 0;                         // rate the hardware port runs at
    int port_type = -1;                        // Vita: SCE_AUDIO_OUT_PORT_TYPE_*
    int port_grain = 0;
    int open_rc = 0;                           // raw return code of the open call
    bool resampling = false;
};
AudioStats audio_stats();

// ============================================================================================ GL (vitaGL on Vita)
// GL threading rule (VITAGL_NOTES.md section 3): every GL call, gl_init and gl_swap must come from ONE thread - the one
// that called gl_init. That thread does not have to be the main thread; the architecture runs it on core 1.
struct GlParams {
    int width = 960, height = 544;
    int msaa = 0;                              // 0 = off, 2 = 2x, 4 = 4x (SCE_GXM_MULTISAMPLE_*), selectable at init
    bool vsync = true;                         // vglWaitVblankStart(vsync); false = uncapped, may tear
    int display_buffers = 3;                   // vglSetDisplayBufferCount (3 = triple buffering, 2 = double)
    // vitaGL memory pools. All sizes in MiB; -1 = vitaGL default behaviour for that pool (everything minus threshold).
    int legacy_pool_bytes = 0;                 // immediate-mode pool; the port does not use glBegin/glEnd -> 0
    int ram_pool_mib = 24;                     // USER_RW_UNCACHE pool for textures/vertex data that do not fit CDRAM
    int cdram_pool_mib = -1;                   // -1: all free CDRAM minus cdram_keep_mib
    int cdram_keep_mib = 0;
    int phycont_pool_mib = 0;                  // USER_MAIN_PHYCONT_NC pool (only SceAvPlayer/SceVideodec need it): 0 = none
    int cdlg_pool_mib = 0;                     // CDIALOG pool (common dialog support) - the port has no dialogs: 0
    int circular_pool_mib = 16;                // vglSetCircularPoolSize: per-frame transient vertex data (client arrays, uniforms-by-copy); vitaGL default is 32 MiB. T8 logs the peak use
    const char* shader_cache_dir = nullptr;    // data-root relative, e.g. "shader_cache"; nullptr = vitaGL default (ux0:data/shader_cache/<TITLE>)
    bool use_cached_mem = false;               // vglUseCachedMem: cached ARM view of vitaGL's RAM pool (needs explicit cache maintenance; leave off)
    bool gc_on_core2 = true;                   // vglSetupGarbageCollector: run the vitaGL GC thread on core 2 at a low priority
    int shark_opt_level = 2;                   // vglSetupRuntimeShaderCompiler(shark_opt, ...): 0 SHARK_OPT_SLOW (O0) 1 SAFE (O1) 2 DEFAULT (O2) 3 FAST (O3) 4 UNSAFE (Ofast); vitaShaRK's own order
    // sceGxm ring buffer sizes (vglSet*BufferSize, must precede vglInit). 0 = keep the vitaGL/sceGxm default
    // (VDM 128 KiB, vertex ring 2 MiB, fragment ring 512 KiB, fragment USSE ring 16 KiB, parameter buffer 16 MiB).
    // Raise vdm_ring_kib / vertex_ring_kib first when a frame has many thousands of draws or per-draw uniform updates.
    int vdm_ring_kib = 0, vertex_ring_kib = 0, fragment_ring_kib = 0, usse_ring_kib = 0, param_buffer_mib = 0;
    // Only meaningful with libvitaGL_fast.a (USE_SCRATCH_MEMORY=1; a no-op in the other variants). GL_STREAM_DRAW / GL_DYNAMIC_DRAW
    // buffers that "want scratch" live in the per-frame circular pool and are overwritten after display_buffers frames, so a
    // persistent GL_DYNAMIC_DRAW ring buffer must NOT use scratch. vitaGL's own default is true/true; ours is false/true.
    bool scratch_for_dynamic = false;
    bool scratch_for_stream = true;
    // Scenes per frame the render targets are created for (vglSetupRenderTargetScenesNum, before vglInit*; GXM's
    // SceGxmRenderTargetParams::scenesPerFrame, "the expected number of scenes per frame"). One for the display target,
    // one for each FBO's target. vitaGL's own default is 1/1. A mission frame draws into some targets twice (the
    // glow chain and the final composite), so 1 is fewer than the frame needs; what going over it costs on the console is
    // not known, and neither is the memory the larger value takes. 0 = keep vitaGL's default for that target; range 0..8.
    int rt_scenes_display = 4, rt_scenes_fbo = 4;
};
// Returns false with a log line when GL cannot start. On Vita a missing libshacccg.suprx is logged loudly
// ("libshacccg.suprx NOT FOUND") but is not fatal by itself: gl_init continues (shaders can still come from a pre-shipped
// shader cache) and returns true; only a shader that is not in the cache then fails to compile. It never crashes.
// Desktop twin: always false ("GL not available in the headless build").
bool gl_init(const GlParams& p);
bool gl_is_ready();
void gl_swap();                                // vglSwapBuffers(GL_FALSE); returns after the frame is queued (see VITAGL_NOTES.md section 4)
void gl_set_vsync(bool on);
void gl_shutdown();                            // vitaGL (9bf3e3f) has NO vglEnd(): this only marks GL as down; memory returns at process exit
void* gl_get_proc(const char* name);           // vglGetProcAddress; nullptr when the function is not implemented
// Vita: true if libshacccg.suprx exists at ur0:/data/libshacccg.suprx (vitaShaRK's default, vitashark.c DEFAULT_SHACCCG_PATH)
// or ur0:data/external/libshacccg.suprx (the path vitaGL's gxm.c passes to shark_init). File existence only.
bool gl_shader_compiler_available();
// Optional per-frame hook that draws on top of the finished frame using the CPU (vglSetDisplayCallback). `fb` points at
// a 960x544 (pitch 960 px) ABGR8888 framebuffer. The callback runs on the display-queue thread: keep it fast.
using GlOverlayFn = void (*)(void* fb, int width, int height, int pitch_pixels);
void gl_set_overlay(GlOverlayFn fn);
// The same overlay composed by the GPU instead: `draw` is called (when `version()` changed) to fill a 960x544 ABGR8888 buffer that starts
// transparent (all zero); gl_swap() uploads it to a texture and draws it as a blended screen quad just before the swap. version() == 0
// means "nothing to show". Works where the CPU-written display buffer is not what ends up on screen (Vita3K shows the GPU's image of
// the render target). The draw leaves vitaGL in a fixed-function state: callers that cache GL state must forget it after gl_swap().
using GlOverlayVersionFn = unsigned (*)();
void gl_set_overlay_composite(GlOverlayFn draw, GlOverlayVersionFn version);

}  // namespace plat
