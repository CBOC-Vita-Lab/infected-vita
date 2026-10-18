// selftest/st_common.h - tiny harness shared by all selftest translation units.
#pragma once
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "../platform/platform.h"

namespace st {

// ---- configuration (ux0:data/infected/selftest.cfg, key=value, '#' comments) --------------------------------------
int cfg_int(const char* key, int def);
const char* cfg_str(const char* key, const char* def);
bool test_enabled(int n);                 // honours skip=T3,T7  only=T8  and t3=0 style keys

// ---- result lines (also mirrored to the on-screen panel and plat::logf) --------------------------------------------
// Result file lines:   PASS|FAIL|INFO|LIMIT|SKIP <test> <name>: <details>
// LIMIT = a known limitation of the stack that is reported but does not fail the run (e.g. glBlendColor absent).
void pass(const char* test, const char* name, const char* fmt, ...) PLAT_PRINTF(3, 4);
void fail(const char* test, const char* name, const char* fmt, ...) PLAT_PRINTF(3, 4);
void info(const char* test, const char* name, const char* fmt, ...) PLAT_PRINTF(3, 4);
void limit(const char* test, const char* name, const char* fmt, ...) PLAT_PRINTF(3, 4);
void skip(const char* test, const char* name, const char* fmt, ...) PLAT_PRINTF(3, 4);
void check(bool ok, const char* test, const char* name, const char* fmt, ...) PLAT_PRINTF(4, 5);
void status(const char* fmt, ...) PLAT_PRINTF(1, 2);   // one-line "what am I doing now" for the screen only
int failures();
int passes();

// ---- START = abort ------------------------------------------------------------------------------------------------
// Polls the pad; if START is held, writes the abort line and exits the process. Cheap enough to call every frame.
void poll_abort(const char* where);

// ---- on-screen text -----------------------------------------------------------------------------------------------
// 8x8 bitmap font (public domain, font8x8_basic) drawn at 2x into an ABGR8888 framebuffer.
void text_draw(uint32_t* fb, int pitch_px, int fbw, int fbh, int x, int y, const char* s, uint32_t abgr, int scale);
// Boot console: CPU framebuffer on the display, used before/after the GL tests (and when GL is unavailable).
bool console_open();
void console_close();
void console_refresh();                                 // redraw the panel on the boot console (no-op when closed)
// Draws the whole panel (opaque background) into a framebuffer: used by the boot console.
void panel_draw(uint32_t* fb, int pitch_px, int fbw, int fbh);
// Draws only the header, the status line and the last `max_lines` result lines on a dark band at the top of a framebuffer
// (no full clear): used from the GL display callback so that the GL test content stays visible below the band.
void panel_draw_overlay(uint32_t* fb, int pitch_px, int fbw, int fbh, int max_lines);

// ---- tests --------------------------------------------------------------------------------------------------------
void t1_clocks();
void t2_memory();
void t3_coroutines();
void t4_threads();
void t5_audio();
void t6_gl_basics();          // runs on the GL thread
void t7_render_to_texture();  // runs on the GL thread
void t8_draw_cost();          // runs on the GL thread
void t9_ffmpeg();
void t10_file_io();

// Defined in t_gl.cpp: one clear+swap so the panel overlay keeps updating while the worker is idle.
void gl_idle_frame();

// GL worker: runs fn on the GL thread (core 1) or inline when gl.thread=0. Blocks until fn returns.
void run_on_gl_thread(void (*fn)());
void gl_worker_stop();

// The 66 MiB guest arena allocated by T2 (kept for the rest of the run so the GL tests run under the real memory budget).
extern plat::MemBlock g_arena;

// Micro helpers
uint32_t crc32_update(uint32_t crc, const void* data, size_t n);

// Defined in t_coro.cpp: freestanding coroutine core (also used by the Unicorn instruction-level test).
struct CoroResult {
    uint32_t switches;        // number of st_co_switch calls performed
    uint32_t errors;          // callee-saved register mismatches (r4-r11, d8-d15) + frame mismatches + stack canary mismatches
    uint32_t last_seen_mask;  // bit i set if coroutine i ran at least once
    uint32_t deep_levels_max; // mode 2: recursion depth reached by coroutine 0 (about 520 bytes of stack per level)
};
// mode 0: star (scheduler <-> coroutine i, `hops` round trips = 2*hops switches)
// mode 1: ring (c0 -> c1 -> c2 -> c3 -> c0 ..., `hops` direct coroutine-to-coroutine switches)
// mode 2: like 1, but the coroutine bodies are C++ (via the trampoline) that recurse deep, switch at the bottom and verify frames
// stack_mem must hold 4 * stack_bytes bytes (8-byte aligned). Returns CoroResult::errors.
extern "C" uint32_t st_coro_run(uint32_t mode, uint32_t hops, uint32_t stack_bytes, void* stack_mem, CoroResult* out);

}  // namespace st
