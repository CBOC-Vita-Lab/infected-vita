// platform/desktop_headless/platform_desktop.cpp - desktop (Linux/macOS) twin of platform/platform.h.
// Headless: no window, GL is stubbed (gl_init returns false), audio goes to a null sink that drains the ring in real time, the
// pad is neutral. Everything else (files, threads, events, logger, memory blocks, clocks) is real so the selftest harness and
// the guest runtime can be exercised on a workstation. Data root: $INFECTED_DATA_ROOT or ./infected_data/ ; app root:
// $INFECTED_APP_ROOT or ./app/ .
#include "../platform.h"

#include <atomic>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>

#if defined(__linux__)
#include <malloc.h>
#endif

namespace plat {

namespace {
char g_data_root[240] = "./infected_data/";
char g_app_root[240] = "./app/";
FILE* g_log = nullptr;
char g_log_path[512] = "";
bool g_log_sync = false;
pthread_mutex_t g_log_mtx = PTHREAD_MUTEX_INITIALIZER;
uint64_t g_t0 = 0;

uint64_t mono_us() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}
int ncpu() {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n < 1 ? 1 : (int)n;
}
}  // namespace

Kind kind() { return Kind::DesktopHeadless; }
const char* name() { return "desktop_headless"; }
uint64_t now_us() { return mono_us() - g_t0; }
void sleep_us(uint32_t us) {
    timespec ts{(time_t)(us / 1000000u), (long)(us % 1000000u) * 1000L};
    nanosleep(&ts, nullptr);
}
void yield() { sched_yield(); }

// ---------------------------------------------------------------- logging
void log_raw(const char* text, size_t len) {
    pthread_mutex_lock(&g_log_mtx);
    if (g_log) {
        fwrite(text, 1, len, g_log);
        fflush(g_log);                       // flush per line: always, like the Vita build
        if (g_log_sync) fsync(fileno(g_log));
    }
    fwrite(text, 1, len, stderr);
    pthread_mutex_unlock(&g_log_mtx);
}

void logf(const char* fmt, ...) {
    char line[1024];
    uint64_t t = now_us();
    int off = snprintf(line, sizeof(line), "[%4u.%03u c%d] ", (unsigned)(t / 1000000u), (unsigned)((t / 1000u) % 1000u), thread_current_core());
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line + off, sizeof(line) - off - 2, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    size_t len = (size_t)off + (size_t)n;
    if (len > sizeof(line) - 2) len = sizeof(line) - 2;
    line[len++] = '\n';
    line[len] = 0;
    log_raw(line, len);
}
void log_flush() {
    pthread_mutex_lock(&g_log_mtx);
    if (g_log) { fflush(g_log); fsync(fileno(g_log)); }
    pthread_mutex_unlock(&g_log_mtx);
}
void log_set_sync(bool on) { g_log_sync = on; }
const char* log_path() { return g_log_path; }
extern "C" int vgl_hook_printf(const char* fmt, ...) {   // same symbol as the Vita build so debug-lib call sites link on both
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n > 0) plat::log_raw(buf, (size_t)(n > (int)sizeof(buf) - 2 ? (int)sizeof(buf) - 2 : n));
    return n;
}

// ---------------------------------------------------------------- paths
bool path_resolve(Root r, const char* rel, char* out, size_t out_size) {
    const char* base = r == Root::Data ? g_data_root : g_app_root;
    if (!rel) rel = "";
    while (*rel == '/') rel++;
    int n = snprintf(out, out_size, "%s%s", base, rel);
    return n >= 0 && (size_t)n < out_size;
}
static bool mkdir_p(const char* full) {
    char tmp[512];
    size_t len = strlen(full);
    if (len >= sizeof(tmp)) return false;
    memcpy(tmp, full, len + 1);
    while (len > 1 && tmp[len - 1] == '/') tmp[--len] = 0;
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = 0; mkdir(tmp, 0777); *p = '/'; }
    }
    if (mkdir(tmp, 0777) == 0) return true;
    struct stat st;
    return stat(tmp, &st) == 0 && S_ISDIR(st.st_mode);
}
bool dir_make(Root r, const char* rel) {
    char full[512];
    return path_resolve(r, rel, full, sizeof(full)) && mkdir_p(full);
}
bool file_exists(Root r, const char* rel) {
    char full[512]; struct stat st;
    return path_resolve(r, rel, full, sizeof(full)) && stat(full, &st) == 0;
}
bool file_remove(Root r, const char* rel) {
    char full[512];
    return path_resolve(r, rel, full, sizeof(full)) && unlink(full) == 0;
}
int64_t file_size_of(Root r, const char* rel) {
    char full[512]; struct stat st;
    if (!path_resolve(r, rel, full, sizeof(full)) || stat(full, &st) != 0) return -1;
    return (int64_t)st.st_size;
}

// ---------------------------------------------------------------- files
struct File {
    int fd = -1;
    FileMode mode = FileMode::Read;
    int64_t phys = 0, size = 0;
    uint8_t* buf = nullptr;
    size_t cap = 0, buf_off = 0, buf_len = 0;
};
File* file_open(Root r, const char* rel, FileMode m, size_t buffer_bytes) {
    char full[512];
    if (!path_resolve(r, rel, full, sizeof(full))) return nullptr;
    int flags = O_RDONLY;
    if (m == FileMode::WriteTruncate) flags = O_WRONLY | O_CREAT | O_TRUNC;
    if (m == FileMode::Append) flags = O_WRONLY | O_CREAT | O_APPEND;
    int fd = open(full, flags, 0666);
    if (fd < 0) return nullptr;
    File* f = new File();
    f->fd = fd;
    f->mode = m;
    off_t end = lseek(fd, 0, SEEK_END);
    if (m == FileMode::Read) { f->size = end < 0 ? 0 : end; lseek(fd, 0, SEEK_SET); }
    else if (m == FileMode::Append) { f->phys = f->size = end < 0 ? 0 : end; }
    else { lseek(fd, 0, SEEK_SET); }
    if (buffer_bytes) { f->buf = (uint8_t*)malloc(buffer_bytes); if (f->buf) f->cap = buffer_bytes; }
    return f;
}
int64_t file_size(File* f) { return f->mode == FileMode::Read ? f->size : f->phys + (int64_t)f->buf_len; }
int64_t file_tell(File* f) { return f->mode == FileMode::Read ? f->phys - (int64_t)(f->buf_len - f->buf_off) : f->phys + (int64_t)f->buf_len; }
bool file_flush(File* f) {
    if (f->mode == FileMode::Read || !f->buf_len) return true;
    ssize_t w = write(f->fd, f->buf, f->buf_len);
    bool ok = w == (ssize_t)f->buf_len;
    f->phys += f->buf_len; f->buf_len = 0;
    return ok;
}
bool file_seek(File* f, int64_t off) {
    if (f->mode != FileMode::Read) {
        if (!file_flush(f)) return false;
        if (lseek(f->fd, off, SEEK_SET) < 0) return false;
        f->phys = off;
        return true;
    }
    int64_t bs = f->phys - (int64_t)f->buf_len;
    if (f->buf_len && off >= bs && off <= f->phys) { f->buf_off = (size_t)(off - bs); return true; }
    if (lseek(f->fd, off, SEEK_SET) < 0) return false;
    f->phys = off; f->buf_off = f->buf_len = 0;
    return true;
}
size_t file_read(File* f, void* dst, size_t n) {
    uint8_t* out = (uint8_t*)dst;
    size_t done = 0;
    while (done < n) {
        if (f->buf_off < f->buf_len) {
            size_t take = f->buf_len - f->buf_off; if (take > n - done) take = n - done;
            memcpy(out + done, f->buf + f->buf_off, take); f->buf_off += take; done += take; continue;
        }
        size_t want = n - done;
        if (!f->buf || want >= f->cap) {
            ssize_t r = read(f->fd, out + done, want);
            if (r <= 0) break;
            f->phys += r; done += (size_t)r;
            if ((size_t)r < want) break;
        } else {
            ssize_t r = read(f->fd, f->buf, f->cap);
            if (r <= 0) break;
            f->phys += r; f->buf_off = 0; f->buf_len = (size_t)r;
        }
    }
    return done;
}
size_t file_write(File* f, const void* src, size_t n) {
    const uint8_t* in = (const uint8_t*)src;
    if (!f->buf) { ssize_t w = write(f->fd, in, n); if (w < 0) return 0; f->phys += w; return (size_t)w; }
    size_t done = 0;
    while (done < n) {
        size_t space = f->cap - f->buf_len, take = n - done < space ? n - done : space;
        memcpy(f->buf + f->buf_len, in + done, take); f->buf_len += take; done += take;
        if (f->buf_len == f->cap && !file_flush(f)) return done;
    }
    return done;
}
void file_close(File* f) {
    if (!f) return;
    file_flush(f); close(f->fd); free(f->buf); delete f;
}
bool file_read_all(Root r, const char* rel, void** data, size_t* size) {
    File* f = file_open(r, rel, FileMode::Read);
    if (!f) return false;
    size_t sz = (size_t)f->size;
    void* p = malloc(sz ? sz : 1);
    size_t got = p ? file_read(f, p, sz) : 0;
    file_close(f);
    if (!p || got != sz) { free(p); return false; }
    *data = p; *size = sz;
    return true;
}
bool file_write_all(Root r, const char* rel, const void* data, size_t size) {
    File* f = file_open(r, rel, FileMode::WriteTruncate);
    if (!f) return false;
    size_t w = file_write(f, data, size);
    file_close(f);
    return w == size;
}
static int copy_rec(const char* s, const char* d, bool overwrite) {
    DIR* dir = opendir(s);
    if (!dir) return -1;
    mkdir_p(d);
    int copied = 0;
    while (dirent* e = readdir(dir)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char ss[512], dd[512];
        snprintf(ss, sizeof(ss), "%s/%s", s, e->d_name);
        snprintf(dd, sizeof(dd), "%s/%s", d, e->d_name);
        struct stat st;
        if (stat(ss, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { int c = copy_rec(ss, dd, overwrite); if (c > 0) copied += c; continue; }
        struct stat st2;
        if (!overwrite && stat(dd, &st2) == 0) continue;
        FILE* in = fopen(ss, "rb");
        FILE* out = in ? fopen(dd, "wb") : nullptr;
        if (in && out) {
            char chunk[65536]; size_t n; bool ok = true;
            while ((n = fread(chunk, 1, sizeof(chunk), in)) > 0) if (fwrite(chunk, 1, n, out) != n) { ok = false; break; }
            if (ok) copied++;
        }
        if (in) fclose(in);
        if (out) fclose(out);
    }
    closedir(dir);
    return copied;
}
int dir_copy_tree(const char* src_rel_app, const char* dst_rel_data, bool overwrite) {
    char s[512], d[512];
    if (!path_resolve(Root::App, src_rel_app, s, sizeof(s)) || !path_resolve(Root::Data, dst_rel_data, d, sizeof(d))) return -1;
    return copy_rec(s, d, overwrite);
}

// ---------------------------------------------------------------- clocks / memory
ClockReport clocks_read() { return ClockReport(); }
int clock_measure_mhz() { return 0; }
int clock_set_arm(int, int* applied_mhz) {
    if (applied_mhz) *applied_mhz = 0;
    return -1;
}
ClockReport clocks_set_max() {
    ClockReport c;
    logf("clocks: desktop build, nothing to set");
    return c;
}
bool mem_info(MemInfo* out) {
#if defined(__linux__)
    struct mallinfo2 mi = mallinfo2();
    out->malloc_arena = (int64_t)mi.arena; out->malloc_used = (int64_t)mi.uordblks; out->malloc_free = (int64_t)mi.fordblks;
    long pages = sysconf(_SC_AVPHYS_PAGES), psz = sysconf(_SC_PAGESIZE);
    if (pages > 0 && psz > 0) out->user_free = (int64_t)pages * psz;
#endif
    return true;
}
bool mem_alloc_block(const char* name, size_t size, size_t align, MemBlock* out) {
    *out = MemBlock();
    if (align < 4096) align = 4096;
    size_t rounded = (size + align - 1) & ~(align - 1);
    size_t total = rounded + align;
    void* raw = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) { logf("mem_alloc_block '%s': mmap %lu failed (%s)", name, (unsigned long)total, strerror(errno)); return false; }
    uintptr_t a = ((uintptr_t)raw + align - 1) & ~(uintptr_t)(align - 1);
    out->base = (void*)a; out->size = rounded; out->raw_base = raw; out->raw_size = total; out->uid = 1;
    logf("mem_alloc_block '%s': %lu KiB at %p", name, (unsigned long)(rounded / 1024), out->base);
    return true;
}
void mem_free_block(MemBlock* b) {
    if (b && b->raw_base) munmap(b->raw_base, b->raw_size);
    if (b) *b = MemBlock();
}
uint64_t mem_touch_pages(void* base, size_t size) {
    uint64_t t0 = now_us();
    volatile uint8_t* p = (volatile uint8_t*)base;
    for (size_t i = 0; i < size; i += 4096) p[i] = p[i];
    return now_us() - t0;
}

// ---------------------------------------------------------------- threads
struct Thread {
    pthread_t th;
    ThreadFn fn;
    void* arg;
    int core;
};
static void pin(pthread_t th, int core) {
#if defined(__linux__)
    if (core < 0) return;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core % ncpu(), &set);
    pthread_setaffinity_np(th, sizeof(set), &set);
#else
    (void)th; (void)core;
#endif
}
static void* thread_tramp(void* p) {
    Thread* t = (Thread*)p;
    t->fn(t->arg);
    return nullptr;
}
Thread* thread_start(const ThreadDesc& d) {
    Thread* t = new Thread();
    t->fn = d.fn; t->arg = d.arg; t->core = d.core;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, d.stack_bytes < 65536 ? 65536 : d.stack_bytes);
    if (pthread_create(&t->th, &attr, thread_tramp, t) != 0) { pthread_attr_destroy(&attr); delete t; return nullptr; }
    pthread_attr_destroy(&attr);
    pin(t->th, d.core);
    return t;
}
void thread_join(Thread* t) { if (!t) return; pthread_join(t->th, nullptr); delete t; }
int thread_current_id() { return (int)(reinterpret_cast<std::uintptr_t>(pthread_self()) & 0x7fffffff); }
int thread_current_core() {
#if defined(__linux__)
    int c = sched_getcpu();
    return c < 0 ? 0 : c % 3;
#else
    return 0;
#endif
}
bool thread_set_core(Thread* t, int core) { pin(t ? t->th : pthread_self(), core); return true; }
void core3_allow(bool) {}
void core3_unlock_allow(bool) {}
bool core3_usable() { return false; }
int helper_core() { return 2; }
bool thread_set_priority(Thread*, int) { return true; }   // no-op on the desktop

struct Mutex { pthread_mutex_t m; };
Mutex* mutex_create(const char*) { Mutex* m = new Mutex(); pthread_mutex_init(&m->m, nullptr); return m; }
void mutex_lock(Mutex* m) { pthread_mutex_lock(&m->m); }
void mutex_unlock(Mutex* m) { pthread_mutex_unlock(&m->m); }
void mutex_destroy(Mutex* m) { if (m) { pthread_mutex_destroy(&m->m); delete m; } }

struct Event {
    pthread_mutex_t m;
    pthread_cond_t c;
    bool sig;
    bool auto_reset;
};
Event* event_create(bool auto_reset, bool initially_signalled) {
    Event* e = new Event();
    pthread_mutex_init(&e->m, nullptr);
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
#if defined(__linux__)
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
#endif
    pthread_cond_init(&e->c, &ca);
    pthread_condattr_destroy(&ca);
    e->sig = initially_signalled; e->auto_reset = auto_reset;
    return e;
}
void event_signal(Event* e) { pthread_mutex_lock(&e->m); e->sig = true; pthread_cond_broadcast(&e->c); pthread_mutex_unlock(&e->m); }
void event_reset(Event* e) { pthread_mutex_lock(&e->m); e->sig = false; pthread_mutex_unlock(&e->m); }
bool event_wait(Event* e, int64_t timeout_us) {
    pthread_mutex_lock(&e->m);
    bool ok = true;
    if (timeout_us < 0) {
        while (!e->sig) pthread_cond_wait(&e->c, &e->m);
    } else if (timeout_us == 0) {
        ok = e->sig;
    } else {
        timespec ts;
#if defined(__linux__)
        clock_gettime(CLOCK_MONOTONIC, &ts);
#else
        clock_gettime(CLOCK_REALTIME, &ts);
#endif
        int64_t ns = ts.tv_nsec + (timeout_us % 1000000) * 1000;
        ts.tv_sec += timeout_us / 1000000 + ns / 1000000000;
        ts.tv_nsec = ns % 1000000000;
        while (!e->sig) {
            if (pthread_cond_timedwait(&e->c, &e->m, &ts) == ETIMEDOUT) break;
        }
        ok = e->sig;
    }
    if (ok && e->auto_reset) e->sig = false;
    pthread_mutex_unlock(&e->m);
    return ok;
}
void event_destroy(Event* e) { if (e) { pthread_mutex_destroy(&e->m); pthread_cond_destroy(&e->c); delete e; } }

// ---------------------------------------------------------------- pad (neutral)
bool touch_corner_tap() { return false; }

bool pad_poll(PadState* out) {
    *out = PadState();
    out->time_us = now_us();
    return true;
}

// ---------------------------------------------------------------- audio (null sink at real-time rate)
namespace {
struct Audio {
    bool open = false;
    int16_t* ring = nullptr;
    uint32_t mask = 0;
    std::atomic<uint32_t> head{0}, tail{0};
    std::atomic<bool> run{false}, armed{false};
    int rate = 44100, grain = 1024;
    Thread* th = nullptr;
    std::atomic<uint64_t> periods{0}, fin{0}, fout{0}, under{0}, under_f{0}, over{0};
    AudioStats st;
} A;
void pump(void*) {
    uint64_t next = mono_us();
    while (A.run.load()) {
        uint32_t avail = A.head.load(std::memory_order_acquire) - A.tail.load(std::memory_order_relaxed);
        uint32_t take = avail < (uint32_t)A.grain ? avail : (uint32_t)A.grain;
        A.tail.fetch_add(take, std::memory_order_release);
        A.fout += take;
        if (take < (uint32_t)A.grain && A.armed.load()) { A.under++; A.under_f += A.grain - take; }
        A.periods++;
        next += (uint64_t)A.grain * 1000000ull / (uint64_t)A.rate;
        uint64_t now = mono_us();
        if (next > now) sleep_us((uint32_t)(next - now));
    }
}
}  // namespace
bool audio_open(const AudioParams& p) {
    if (A.open) return true;
    if (p.ring_frames < 1024 || (p.ring_frames & (p.ring_frames - 1))) return false;
    A.ring = (int16_t*)calloc((size_t)p.ring_frames, 4);
    A.mask = (uint32_t)p.ring_frames - 1;
    A.head = A.tail = 0; A.rate = p.sample_rate; A.grain = (p.period_frames + 63) & ~63;
    A.periods = A.fin = A.fout = A.under = A.under_f = A.over = 0;
    A.armed = false; A.run = true;
    A.st = AudioStats(); A.st.port_rate = p.sample_rate; A.st.port_grain = A.grain; A.st.port_type = 0;
    ThreadDesc d; d.name = "plat-audio"; d.fn = pump; d.core = p.core; d.priority = p.priority;
    A.th = thread_start(d);
    A.open = A.th != nullptr;
    logf("audio_open (null sink): %d Hz grain %d ring %d -> %d", p.sample_rate, A.grain, p.ring_frames, (int)A.open);
    return A.open;
}
void audio_close() {
    if (!A.open) return;
    A.run = false; thread_join(A.th); A.th = nullptr; free(A.ring); A.ring = nullptr; A.open = false;
}
bool audio_is_open() { return A.open; }
bool audio_set_source(AudioSource, AudioFill) { return false; }   // the null sink keeps the push model: desktop runs are paced by the game's clock, not the host's
size_t audio_write(const int16_t* in, size_t frames) {
    if (!A.open) return 0;
    uint32_t h = A.head.load(std::memory_order_relaxed), t = A.tail.load(std::memory_order_acquire);
    uint32_t freef = (A.mask + 1) - (h - t);
    size_t n = frames < freef ? frames : freef;
    for (size_t i = 0; i < n; i++) {
        uint32_t idx = (h + (uint32_t)i) & A.mask;
        A.ring[2 * idx] = in[2 * i]; A.ring[2 * idx + 1] = in[2 * i + 1];
    }
    A.head.store(h + (uint32_t)n, std::memory_order_release);
    A.fin += n;
    if (n) A.armed = true;
    if (n < frames) A.over++;
    return n;
}
size_t audio_free_frames() { return A.open ? (A.mask + 1) - (A.head.load() - A.tail.load()) : 0; }
size_t audio_queued_frames() { return A.open ? A.head.load() - A.tail.load() : 0; }
void audio_set_volume(float) {}
AudioStats audio_stats() {
    AudioStats s = A.st;
    s.periods_out = A.periods; s.frames_in = A.fin; s.frames_out = A.fout; s.underruns = A.under; s.underrun_frames = A.under_f; s.overruns = A.over;
    return s;
}

// ---------------------------------------------------------------- GL (stub)
bool gl_init(const GlParams&) { logf("GL: not available in the desktop_headless build (stub)"); return false; }
bool gl_is_ready() { return false; }
void gl_swap() {}
void gl_set_vsync(bool) {}
void gl_shutdown() {}
void* gl_get_proc(const char*) { return nullptr; }
bool gl_shader_compiler_available() { return false; }
void gl_set_overlay(GlOverlayFn) {}

// ---------------------------------------------------------------- lifecycle
bool init(const InitParams& p) {
    g_t0 = mono_us();
    if (const char* e = getenv("INFECTED_DATA_ROOT")) { snprintf(g_data_root, sizeof(g_data_root), "%s%s", e, e[strlen(e) - 1] == '/' ? "" : "/"); }
    if (const char* e = getenv("INFECTED_APP_ROOT")) { snprintf(g_app_root, sizeof(g_app_root), "%s%s", e, e[strlen(e) - 1] == '/' ? "" : "/"); }
    bool ok = dir_make(Root::Data, "");
    if (p.log_file) {
        char full[512];
        path_resolve(Root::Data, p.log_file, full, sizeof(full));
        snprintf(g_log_path, sizeof(g_log_path), "%s", full);
        g_log = fopen(full, "w");
        g_log_sync = p.log_sync_each_line;
    }
    logf("platform init: desktop_headless, data root %s, log %s", g_data_root, g_log_path[0] ? g_log_path : "(none)");
    return ok;
}
void shutdown() {
    audio_close();
    logf("platform shutdown");
    pthread_mutex_lock(&g_log_mtx);
    if (g_log) { fclose(g_log); g_log = nullptr; }
    pthread_mutex_unlock(&g_log_mtx);
}
void exit_process(int code) { _exit(code); }

}  // namespace plat
