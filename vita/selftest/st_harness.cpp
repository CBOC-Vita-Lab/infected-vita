// selftest/st_harness.cpp - entry point, configuration, result file, on-screen panel, START handling, GL worker thread.
#include "st_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "font8x8_basic.h"

#if defined(__vita__)
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
// Memory configuration of the app (see vita/README.md section "Memory" and VITAGL_NOTES.md sections 2-3):
//   newlib heap: default is 128 MiB; the port needs ~130 MiB of its own heap, T2 allocates 96 MiB + the 66 MiB arena, so ask for 144 MiB.
extern "C" {
unsigned int _newlib_heap_size_user = 144 * 1024 * 1024;
// vita-elf-create copies these (by symbol name) into the module's process-param block. In C++ a namespace-scope const has
// internal linkage unless declared extern, so every one of them is spelled `extern const` here.
extern const char sceUserMainThreadName[] = "infected_selftest";
extern const int sceUserMainThreadPriority = 0x10000100;          // SCE_KERNEL_DEFAULT_PRIORITY_USER encoding; UNVERIFIED that raw values like 0x60 (samples) are equivalent
extern const unsigned int sceUserMainThreadStackSize = 1024 * 1024;
extern const unsigned int sceUserMainThreadCpuAffinityMask = 0x10000;   // SCE_KERNEL_CPU_MASK_USER_0: the "guest executor" core
}
#endif

namespace st {

// =========================================================================================================== config
namespace {
struct KV { char k[48]; char v[96]; };
KV g_cfg[96];
int g_cfg_n = 0;

void cfg_load() {
    void* data = nullptr;
    size_t size = 0;
    if (!plat::file_read_all(plat::Root::Data, "selftest.cfg", &data, &size)) {
        plat::logf("selftest.cfg not found: using defaults (create ux0:data/infected/selftest.cfg with key=value lines to override)");
        return;
    }
    char* p = (char*)data;
    size_t i = 0;
    while (i < size && g_cfg_n < 96) {
        size_t e = i;
        while (e < size && p[e] != '\n' && p[e] != '\r') e++;
        // line = [i, e)
        size_t s = i;
        while (s < e && (p[s] == ' ' || p[s] == '\t')) s++;
        if (s < e && p[s] != '#') {
            size_t eq = s;
            while (eq < e && p[eq] != '=') eq++;
            if (eq < e) {
                size_t ke = eq;
                while (ke > s && (p[ke - 1] == ' ' || p[ke - 1] == '\t')) ke--;
                size_t vs = eq + 1;
                while (vs < e && (p[vs] == ' ' || p[vs] == '\t')) vs++;
                size_t ve = e;
                while (ve > vs && (p[ve - 1] == ' ' || p[ve - 1] == '\t')) ve--;
                KV& kv = g_cfg[g_cfg_n++];
                size_t kl = ke - s < sizeof(kv.k) - 1 ? ke - s : sizeof(kv.k) - 1;
                size_t vl = ve - vs < sizeof(kv.v) - 1 ? ve - vs : sizeof(kv.v) - 1;
                memcpy(kv.k, p + s, kl); kv.k[kl] = 0;
                memcpy(kv.v, p + vs, vl); kv.v[vl] = 0;
                for (char* c = kv.k; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
                plat::logf("cfg: %s = %s", kv.k, kv.v);
            }
        }
        i = e;
        while (i < size && (p[i] == '\n' || p[i] == '\r')) i++;
    }
    free(data);
}

bool list_has(const char* list, int n) {
    // list like "T3,t7, 8"
    const char* p = list;
    while (*p) {
        while (*p == ' ' || *p == ',' || *p == 'T' || *p == 't') p++;
        if (*p >= '0' && *p <= '9') {
            int v = 0;
            while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
            if (v == n) return true;
        } else if (*p) p++;
    }
    return false;
}
}  // namespace

const char* cfg_str(const char* key, const char* def) {
    for (int i = 0; i < g_cfg_n; i++)
        if (!strcmp(g_cfg[i].k, key)) return g_cfg[i].v;
    return def;
}
int cfg_int(const char* key, int def) {
    const char* s = cfg_str(key, nullptr);
    return s ? atoi(s) : def;
}
bool test_enabled(int n) {
    char key[8];
    snprintf(key, sizeof(key), "t%d", n);
    const char* v = cfg_str(key, nullptr);
    if (v && atoi(v) == 0) return false;
    const char* only = cfg_str("only", nullptr);
    if (only && *only) return list_has(only, n);
    const char* skp = cfg_str("skip", nullptr);
    if (skp && list_has(skp, n)) return false;
    return true;
}

// =========================================================================================================== results + panel
namespace {
plat::File* g_res = nullptr;
int g_fail = 0, g_pass = 0, g_limit = 0;
plat::Mutex* g_mtx = nullptr;
constexpr int kLines = 30;
char g_lines[kLines][124];
uint32_t g_line_col[kLines];
int g_head = 0;       // next slot
char g_status[124] = "starting";
char g_title[124] = "";

void ring_add(const char* line, uint32_t col) {
    plat::mutex_lock(g_mtx);
    // split long lines at 58 characters so the 60-column screen shows everything
    size_t n = strlen(line);
    size_t off = 0;
    bool first = true;
    do {
        char tmp[124];
        size_t take = n - off > 58 ? 58 : n - off;
        snprintf(tmp, sizeof(tmp), "%s%.*s", first ? "" : "  ", (int)take, line + off);
        snprintf(g_lines[g_head], sizeof(g_lines[0]), "%s", tmp);
        g_line_col[g_head] = col;
        g_head = (g_head + 1) % kLines;
        off += take;
        first = false;
    } while (off < n);
    plat::mutex_unlock(g_mtx);
}

void emit(const char* kind, const char* test, const char* name, const char* fmt, va_list ap) {
    char details[640];
    vsnprintf(details, sizeof(details), fmt, ap);
    char line[760];
    int n = snprintf(line, sizeof(line), "%-5s %s %s: %s\n", kind, test, name, details);
    if (n < 0) return;
    if (g_res) {
        plat::file_write(g_res, line, (size_t)n);   // unbuffered File: one sceIoWrite per line = flushed per line
    }
    size_t l = strlen(line);
    if (l && line[l - 1] == '\n') line[l - 1] = 0;
    plat::logf("%s", line);   // the log gets it too
    uint32_t col = 0xFFFFFFFF;
    if (!strcmp(kind, "PASS")) col = 0xFF60FF60;
    else if (!strcmp(kind, "FAIL")) col = 0xFF4848FF;
    else if (!strcmp(kind, "LIMIT")) col = 0xFF40C8FF;
    else if (!strcmp(kind, "SKIP")) col = 0xFFA0A0A0;
    else col = 0xFFE0E0E0;
    ring_add(line, col);
    console_refresh();
}
}  // namespace

#define DEFINE_REPORT(fn, kind, counter)                         \
    void fn(const char* test, const char* name, const char* fmt, ...) { \
        va_list ap; va_start(ap, fmt); emit(kind, test, name, fmt, ap); va_end(ap); counter; }
DEFINE_REPORT(pass, "PASS", g_pass++)
DEFINE_REPORT(fail, "FAIL", g_fail++)
DEFINE_REPORT(info, "INFO", (void)0)
DEFINE_REPORT(limit, "LIMIT", g_limit++)
DEFINE_REPORT(skip, "SKIP", (void)0)

void check(bool ok, const char* test, const char* name, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    emit(ok ? "PASS" : "FAIL", test, name, fmt, ap);
    va_end(ap);
    if (ok) g_pass++; else g_fail++;
}
int failures() { return g_fail; }
int passes() { return g_pass; }

void status(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    plat::mutex_lock(g_mtx);
    vsnprintf(g_status, sizeof(g_status), fmt, ap);
    plat::mutex_unlock(g_mtx);
    va_end(ap);
    console_refresh();
}

void panel_draw(uint32_t* fb, int pitch, int w, int h) {
    for (int y = 0; y < h; y++) {
        uint32_t* row = fb + (size_t)y * pitch;
        for (int x = 0; x < w; x++) row[x] = 0xFF181018;   // dark background (ABGR)
    }
    if (!g_mtx) return;
    plat::mutex_lock(g_mtx);
    char head[220];
    snprintf(head, sizeof(head), "%s  pass %d  fail %d  limit %d", g_title, g_pass, g_fail, g_limit);
    text_draw(fb, pitch, w, h, 8, 4, head, g_fail ? 0xFF4848FF : 0xFFFFFFFF, 2);
    text_draw(fb, pitch, w, h, 8, 22, g_status, 0xFF40FFFF, 2);
    int y = 44;
    for (int i = 0; i < kLines; i++) {
        int idx = (g_head + i) % kLines;
        if (!g_lines[idx][0]) continue;
        text_draw(fb, pitch, w, h, 8, y, g_lines[idx], g_line_col[idx], 2);
        y += 16;
        if (y > h - 18) break;
    }
    plat::mutex_unlock(g_mtx);
}

void panel_draw_overlay(uint32_t* fb, int pitch, int w, int h, int max_lines) {
    if (!g_mtx) return;
    int band_h = 44 + max_lines * 16 + 4;
    if (band_h > h) band_h = h;
    for (int y = 0; y < band_h; y++) {
        uint32_t* row = fb + (size_t)y * pitch;
        for (int x = 0; x < w; x++) row[x] = 0xFF181018;
    }
    plat::mutex_lock(g_mtx);
    char head[220];
    snprintf(head, sizeof(head), "%s  pass %d  fail %d  limit %d", g_title, g_pass, g_fail, g_limit);
    text_draw(fb, pitch, w, h, 8, 4, head, g_fail ? 0xFF4848FF : 0xFFFFFFFF, 2);
    text_draw(fb, pitch, w, h, 8, 22, g_status, 0xFF40FFFF, 2);
    int y = 44;
    int start = g_head - max_lines;
    for (int i = 0; i < max_lines; i++) {
        int idx = ((start + i) % kLines + kLines) % kLines;
        if (!g_lines[idx][0]) continue;
        text_draw(fb, pitch, w, h, 8, y, g_lines[idx], g_line_col[idx], 2);
        y += 16;
    }
    plat::mutex_unlock(g_mtx);
}

// =========================================================================================================== text
void text_draw(uint32_t* fb, int pitch, int fbw, int fbh, int x0, int y0, const char* s, uint32_t col, int scale) {
    int x = x0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c >= 128) c = '?';
        const unsigned char* g = font8x8_basic[c];
        for (int ry = 0; ry < 8; ry++) {
            for (int rx = 0; rx < 8; rx++) {
                if (!(g[ry] & (1 << rx))) continue;
                for (int sy = 0; sy < scale; sy++) {
                    int py = y0 + ry * scale + sy;
                    if (py < 0 || py >= fbh) continue;
                    uint32_t* row = fb + (size_t)py * pitch;
                    for (int sx = 0; sx < scale; sx++) {
                        int px = x + rx * scale + sx;
                        if (px >= 0 && px < fbw) row[px] = col;
                    }
                }
            }
        }
        x += 8 * scale;
        if (x >= fbw) break;
    }
}

uint32_t crc32_update(uint32_t crc, const void* data, size_t n) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    const uint8_t* p = (const uint8_t*)data;
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

// =========================================================================================================== GL worker thread
namespace {
struct Worker {
    plat::Thread* th = nullptr;
    plat::Event* wake = nullptr;
    plat::Event* done = nullptr;
    void (*job)() = nullptr;
    volatile bool quit = false;
    bool inline_mode = false;
} W;

void worker_main(void*) {
    plat::logf("GL worker thread up (core %d)", plat::thread_current_core());
    while (!W.quit) {
        bool got = plat::event_wait(W.wake, 50 * 1000);
        if (W.quit) break;
        if (got && W.job) {
            void (*j)() = W.job;
            W.job = nullptr;
            j();
            plat::event_signal(W.done);
        } else if (plat::gl_is_ready()) {
            gl_idle_frame();
        }
    }
}
}  // namespace

void run_on_gl_thread(void (*fn)()) {
    if (cfg_int("gl.thread", 1) == 0) {
        fn();
        return;
    }
    if (!W.th) {
        W.wake = plat::event_create(true);
        W.done = plat::event_create(true);
        plat::ThreadDesc d;
        d.name = "gl-worker";
        d.fn = worker_main;
        d.core = 1;
        d.stack_bytes = 512 * 1024;
        W.th = plat::thread_start(d);
        if (!W.th) {
            fail("T0", "gl-worker", "could not start the GL worker thread; running GL tests inline on the main thread");
            fn();
            return;
        }
    }
    W.job = fn;
    plat::event_signal(W.wake);
    plat::event_wait(W.done, -1);
}

void gl_worker_stop() {
    if (!W.th) return;
    W.quit = true;
    plat::event_signal(W.wake);
    plat::thread_join(W.th);
    W.th = nullptr;
}

void poll_abort(const char* where) {
    plat::PadState p;
    if (plat::pad_poll(&p) && (p.buttons & plat::PAD_START)) {
        info("T0", "abort", "START pressed (%s): leaving now", where);
        if (g_res) { plat::file_close(g_res); g_res = nullptr; }
        plat::logf("selftest aborted by START at %s", where);
        plat::shutdown();
        plat::exit_process(2);
    }
}

}  // namespace st

// =========================================================================================================== main
#ifndef ST_VARIANT
#define ST_VARIANT "release"
#endif
#ifndef ST_FFMPEG_VARIANT
#define ST_FFMPEG_VARIANT "n/a"
#endif
#ifndef ST_ATTR2
#define ST_ATTR2 (-1)   // not a Vita package (desktop twin)
#endif

int main(int, char**) {
    using namespace st;
    plat::InitParams ip;
    ip.log_file = "log.txt";
    plat::init(ip);
    g_mtx = plat::mutex_create("st-panel");
    snprintf(g_title, sizeof(g_title), "Infected Port Self-Test");
    cfg_load();
    if (cfg_int("log_sync", 0)) {
        plat::log_set_sync(true);   // log.txt (which mirrors every result line) is fsync'ed per line: survives a console freeze, slower
        plat::logf("log_sync=1: log.txt is synced after every line");
    }

    g_res = plat::file_open(plat::Root::Data, "selftest.txt", plat::FileMode::WriteTruncate, 0);
    if (!g_res) plat::logf("ERROR: cannot create selftest.txt");
    console_open();
    info("T0", "build", "infected_selftest, %s %s, platform %s, vitaGL variant %s, FFmpeg variant %s, param.sfo ATTRIBUTE2 %d (this package's build setting; "
         "what the OS grants is in memory-at-start)", __DATE__, __TIME__, plat::name(), ST_VARIANT, ST_FFMPEG_VARIANT, ST_ATTR2);
    {
        plat::MemInfo mi;
        plat::mem_info(&mi);
        info("T0", "memory-at-start", "user free %lld KiB, cdram free %lld KiB, phycont free %lld KiB, malloc arena %lld KiB",
             (long long)(mi.user_free / 1024), (long long)(mi.cdram_free / 1024), (long long)(mi.phycont_free / 1024), (long long)(mi.malloc_arena / 1024));
    }
    poll_abort("start");

    struct T { int n; const char* name; void (*fn)(); bool gl; };
    const T tests[] = {
        {1, "clocks", t1_clocks, false},
        {2, "memory", t2_memory, false},
        {3, "coroutines", t3_coroutines, false},
        {4, "threads", t4_threads, false},
        {5, "audio", t5_audio, false},
        {6, "gl-basics", t6_gl_basics, true},
        {7, "render-to-texture", t7_render_to_texture, true},
        {8, "draw-call-cost", t8_draw_cost, true},
        {9, "ffmpeg", t9_ffmpeg, false},
        {10, "file-io", t10_file_io, false},
    };
    for (const T& t : tests) {
        char tag[8];
        snprintf(tag, sizeof(tag), "T%d", t.n);
        if (!test_enabled(t.n)) {
            skip(tag, t.name, "disabled by selftest.cfg");
            continue;
        }
        status("%s %s ...", tag, t.name);
        info(tag, "begin", "%s", t.name);
        uint64_t t0 = plat::now_us();
        if (t.gl) run_on_gl_thread(t.fn); else t.fn();
        info(tag, "end", "%s done in %llu ms", t.name, (unsigned long long)((plat::now_us() - t0) / 1000));
        poll_abort(tag);
    }

    // ---- summary
    bool ok = failures() == 0;
    char sum[160];
    snprintf(sum, sizeof(sum), "RESULT: %s  (%d passed, %d failed)", ok ? "PASS" : "FAIL", passes(), failures());
    info("T0", "summary", "%s", sum);
    status("%s - press START to exit", sum);
    if (g_res) {
        plat::file_close(g_res);
        g_res = nullptr;
    }
    // keep the summary on screen for exit_delay_ms (default 4 s) or until START
    int delay_ms = cfg_int("exit_delay_ms", 4000);
    uint64_t end = plat::now_us() + (uint64_t)delay_ms * 1000;
    while (plat::now_us() < end) {
        plat::PadState p;
        if (plat::pad_poll(&p) && (p.buttons & plat::PAD_START)) break;
        plat::sleep_us(20000);
    }
    gl_worker_stop();
    plat::shutdown();
    plat::exit_process(ok ? 0 : 1);
    return 0;
}
