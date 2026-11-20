#include "app_stdio_lock.h"

#include <atomic>
#include <stdarg.h>
#include <stdio.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

// The real functions, which the linker names __real_X when X is wrapped (see CMakeLists.txt).
extern "C" {
int __real_printf(const char*, ...);
int __real_vprintf(const char*, va_list);
int __real_puts(const char*);
int __real_putchar(int);
int __real_fprintf(FILE*, const char*, ...);
int __real_vfprintf(FILE*, const char*, va_list);
int __real_fputs(const char*, FILE*);
int __real_fputc(int, FILE*);
int __real_putc(int, FILE*);
size_t __real_fwrite(const void*, size_t, size_t, FILE*);
int __real_fflush(FILE*);
FILE* __real_fopen(const char*, const char*);
FILE* __real_freopen(const char*, const char*, FILE*);
FILE* __real_fdopen(int, const char*);
int __real_fclose(FILE*);
}

namespace app {
namespace {

// A recursive lock made of a light-weight mutex and the owner's thread id (libc calls that are themselves wrapped, such as
// assert's fprintf, may nest). Created on first use: stdio is used before main() runs.
SceKernelLwMutexWork g_mtx;
std::atomic<int> g_state{0};
std::atomic<int> g_owner{0};   // thread id of the holder: the lock is made recursive here, not left to the attribute
int g_depth = 0;   // 0 not made, 1 being made, 2 ready, 3 could not be made (no locking then)
bool g_stdio_on = true;
bool g_fopen_on = true;
std::atomic<uint64_t> g_locked{0}, g_opens{0};
std::atomic<uint32_t> g_max_wait{0}, g_max_hold_open{0};

bool ready() {
    int st = g_state.load(std::memory_order_acquire);
    if (st == 2) return true;
    if (st == 3) return false;
    int zero = 0;
    if (g_state.compare_exchange_strong(zero, 1)) {
        g_state.store(sceKernelCreateLwMutex(&g_mtx, "app-stdio", 0, 0, nullptr) >= 0 ? 2 : 3, std::memory_order_release);
    } else {
        while (g_state.load(std::memory_order_acquire) == 1) sceKernelDelayThread(10);
    }
    return g_state.load(std::memory_order_acquire) == 2;
}

struct Guard {
    bool held = false;
    bool nested = false;
    uint64_t t0 = 0;
    bool timed;
    explicit Guard(bool enabled, bool time_hold = false) : timed(time_hold) {
        if (!enabled || !ready()) return;
        const int me = (int)sceKernelGetThreadId();
        if (g_owner.load(std::memory_order_acquire) == me) {   // nested (a wrapped call inside a wrapped call)
            g_depth++;
            nested = true;
            held = true;
            return;
        }
        const uint64_t w0 = sceKernelGetProcessTimeWide();
        sceKernelLockLwMutex(&g_mtx, 1, nullptr);
        g_owner.store(me, std::memory_order_release);
        g_depth = 1;
        const uint32_t waited = (uint32_t)(sceKernelGetProcessTimeWide() - w0);
        if (waited > g_max_wait.load(std::memory_order_relaxed)) g_max_wait.store(waited, std::memory_order_relaxed);
        held = true;
        t0 = sceKernelGetProcessTimeWide();
    }
    ~Guard() {
        if (!held) return;
        if (nested) {
            g_depth--;
            return;
        }
        if (timed) {
            const uint32_t hold = (uint32_t)(sceKernelGetProcessTimeWide() - t0);
            if (hold > g_max_hold_open.load(std::memory_order_relaxed)) g_max_hold_open.store(hold, std::memory_order_relaxed);
        }
        g_depth = 0;
        g_owner.store(0, std::memory_order_release);
        sceKernelUnlockLwMutex(&g_mtx, 1);
    }
};


bool stdio_on() { return g_stdio_on; }
bool fopen_on() { return g_fopen_on; }
void count_locked() { g_locked.fetch_add(1, std::memory_order_relaxed); }
void count_open() { g_opens.fetch_add(1, std::memory_order_relaxed); }

}  // namespace

void stdio_lock_configure(bool stdio_on, bool fopen_on) {
    g_stdio_on = stdio_on;
    g_fopen_on = fopen_on;
}
StdioLockStats stdio_lock_stats() {
    StdioLockStats s;
    s.locked_calls = g_locked.load();
    s.open_calls = g_opens.load();
    s.max_wait_us = g_max_wait.load();
    s.max_hold_open_us = g_max_hold_open.load();
    return s;
}

}  // namespace app

#define W app::Guard guard(app::stdio_on()); app::count_locked()
#define O app::Guard guard(app::fopen_on(), true); app::count_open()

extern "C" {
int __wrap_printf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    W;
    const int r = __real_vprintf(fmt, ap);
    va_end(ap);
    return r;
}
int __wrap_vprintf(const char* fmt, va_list ap) { W; return __real_vprintf(fmt, ap); }
int __wrap_puts(const char* s) { W; return __real_puts(s); }
int __wrap_putchar(int c) { W; return __real_putchar(c); }
int __wrap_fprintf(FILE* f, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    W;
    const int r = __real_vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}
int __wrap_vfprintf(FILE* f, const char* fmt, va_list ap) { W; return __real_vfprintf(f, fmt, ap); }
int __wrap_fputs(const char* s, FILE* f) { W; return __real_fputs(s, f); }
int __wrap_fputc(int c, FILE* f) { W; return __real_fputc(c, f); }
int __wrap_putc(int c, FILE* f) { W; return __real_putc(c, f); }
size_t __wrap_fwrite(const void* p, size_t size, size_t n, FILE* f) { W; return __real_fwrite(p, size, n, f); }
int __wrap_fflush(FILE* f) { W; return __real_fflush(f); }
FILE* __wrap_fopen(const char* path, const char* mode) { O; return __real_fopen(path, mode); }
FILE* __wrap_freopen(const char* path, const char* mode, FILE* f) { O; return __real_freopen(path, mode, f); }
FILE* __wrap_fdopen(int fd, const char* mode) { O; return __real_fdopen(fd, mode); }
int __wrap_fclose(FILE* f) { O; return __real_fclose(f); }
}
