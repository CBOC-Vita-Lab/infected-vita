// platform/vita/platform_vita.cpp - PS Vita implementation of platform/platform.h (core, files, threads, clocks, memory, pad).
// Audio lives in audio_vita.cpp, GL in gl_vita.cpp. All SDK signatures used here were checked against
// $VITASDK/arm-vita-eabi/include (see LOG.md); anything not checkable offline is marked UNVERIFIED.
#include "../platform.h"
#include "../file_replace.h"
#include "../log_queue.h"
#include "../retry.h"

#include <atomic>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/appmgr.h>
#include <psp2/ctrl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/error.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/modulemgr.h>
#include <taihen.h>
#include <psp2/power.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/touch.h>

namespace plat {

static const char kDataRoot[] = "ux0:data/infected/";
static const char kAppRoot[] = "app0:";

// ------------------------------------------------------------------------------------------------ small helpers
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// User thread priorities: 0x10000100 is "default" (maps to 160); +-32 around it is the accepted window in Vita3K.
static int prio_abs(int rel) { return 0x10000100 + clampi(rel, -32, 31); }
static const int kCpu3Mask = 0x80000;   // CPU 0..2 are 0x10000 << n (SCE_KERNEL_CPU_MASK_USER_n); CPU 3 follows the same pattern
static int g_core3_state = -1;           // -1: not probed yet, 0: no, 1: usable
static bool g_core3_allowed = true;
static bool g_core3_unlock_allowed = true;
static SceUID g_core3_module = -1;        // infected_core3.skprx while this application has it loaded
static bool g_core3_wrote_pending = false;
static int core_mask(int core);

// Starts a thread on the CPU 3 mask and reads back where it ran: the plugin lifting the restriction lets the call through, without
// it the kernel refuses the mask (or places the thread elsewhere, which counts as no).
static volatile int g_probe_cpu = -1;
static int core3_probe_main(SceSize, void*) {
    g_probe_cpu = (int)sceKernelGetCpuId();
    return 0;
}
static bool core3_probe_once() {
    g_probe_cpu = -1;
    SceUID id = sceKernelCreateThread("core3-probe", core3_probe_main, prio_abs(0), 0x4000, 0, kCpu3Mask, nullptr);
    if (id < 0) {
        logf("core3: a thread on the CPU 3 mask is refused (0x%08X): the fourth core is reserved for the system here", (unsigned)id);
        return false;
    }
    int rc = sceKernelStartThread(id, 0, nullptr);
    int ok = 0;
    if (rc >= 0) {
        int stat = 0;
        SceUInt timeout = 500000;
        if (sceKernelWaitThreadEnd(id, &stat, &timeout) >= 0) ok = 1;
    }
    sceKernelDeleteThread(id);
    const bool usable = ok && g_probe_cpu == 3;
    logf("core3: probe thread ran on CPU %d (start rc=0x%08X): %s", g_probe_cpu, (unsigned)rc,
         usable ? "the fourth core is usable" : "not usable, helper threads stay on core 2");
    return usable;
}

// One probe can miss for a moment (the thread placed elsewhere, a slow start): it is tried up to g_core3_retries times, a few
// milliseconds apart, and the number of tries is logged.
static int g_core3_retries = 6;
static int g_core3_tries = 0;
static bool core3_probe() {
    int tried = 0;
    const int won = retry_probe(g_core3_retries < 1 ? 1 : g_core3_retries, [](int) { return core3_probe_once(); },
                                [](int n) { logf("core3: probe try %d failed, trying again", n); sceKernelDelayThread(8000); }, &tried);
    g_core3_tries = tried;
    if (tried > 1) logf("core3: probe %s after %d tries", won ? "worked" : "did not work", tried);
    return won != 0;
}

// The package's own kernel module (vita/core3/core3.c) that lets the CPU 3 mask through: loaded from app0: through taiHEN, which every
// HENkaku/Ensō console has, so nothing has to be installed. It starts only on firmware 3.60 to 3.74 (it checks the real version itself)
// and is unloaded by shutdown(). A console that already allows core 3 (CapUnlocker installed) never loads it.
// A start that loads the module leaves core3_unlock.pending in the data folder until the fourth core has been seen working; a start that
// finds the file (the previous one never got that far) does not load the module again, so a console the module upsets keeps starting.
// Deleting the file tries again.
static const char kCore3Pending[] = "core3_unlock.pending";
static bool core3_unlock() {
    if (file_exists(Root::Data, kCore3Pending)) {
        logf("core3: %s is there (an earlier start loaded the core-3 module and did not finish): not loading it", kCore3Pending);
        return false;
    }
    // The module patches one firmware build's code: it is only offered to the systems it was made for (3.60-3.74, which the
    // module checks again itself). Anything else (another firmware, or an emulator that answers with nothing) is left alone.
    SceKernelSystemSwVersion version;
    memset(&version, 0, sizeof(version));
    version.size = sizeof(version);
    const int vrc = sceKernelGetSystemSwVersion(&version);
    version.versionString[sizeof(version.versionString) - 1] = '\0';
    if (vrc < 0 || version.version < 0x03600000u || version.version >= 0x03750000u) {
        logf("core3: system software 0x%08X (rc 0x%08X) is not one the core-3 module is made for: not loading it",
             (unsigned)version.version, (unsigned)vrc);
        return false;
    }
    logf("core3: system software %s (0x%08X)", version.versionString, (unsigned)version.version);
    g_core3_wrote_pending = file_write_all(Root::Data, kCore3Pending, "1\n", 2);
    // The kernel cannot see app0: (a mount of this process only: 23 Oct console, 0x80010013), so the module is loaded from the
    // application's real folder, ux0:app/<title id>/.
    char title[16] = {0};
    char path[96];
    SceIoStat st;
    if (sceAppMgrAppParamGetString(0, 12, title, sizeof(title)) >= 0 && title[0] != '\0') {
        snprintf(path, sizeof(path), "ux0:app/%s/module/infected_core3.skprx", title);
        if (sceIoGetstat(path, &st) < 0) snprintf(path, sizeof(path), "app0:module/infected_core3.skprx");
    } else {
        snprintf(path, sizeof(path), "app0:module/infected_core3.skprx");
    }
    logf("core3: loading %s", path);
    const SceUID id = taiLoadStartKernelModule(path, 0, nullptr, 0);
    if (id < 0) {
        logf("core3: the package's core-3 module did not start (0x%08X): helper threads stay on core 2", (unsigned)id);
        return false;
    }
    g_core3_module = id;
    logf("core3: the package's core-3 module is loaded (id 0x%08X)", (unsigned)id);
    return true;
}

static void core3_release() {
    if (g_core3_module < 0) return;
    int status = 0;
    const int rc = taiStopUnloadKernelModule(g_core3_module, 0, nullptr, 0, nullptr, &status);
    logf("core3: the package's core-3 module is unloaded (rc 0x%08X)", (unsigned)rc);
    g_core3_module = -1;
}

void core3_allow(bool allowed) { g_core3_allowed = allowed; }
void core3_unlock_allow(bool allowed) { g_core3_unlock_allowed = allowed; }
bool core3_usable() {
    if (!g_core3_allowed) return false;
    if (g_core3_state < 0) {
        bool usable = core3_probe();
        if (!usable && g_core3_unlock_allowed && core3_unlock()) {
            usable = core3_probe();
            if (!usable) core3_release();   // loaded but the mask is still refused: nothing gained, nothing left behind
        }
        // This start loaded the module and lived through the probe: the next one may load it again. (A file an earlier start left
        // behind stays: core3_unlock() saw it and loaded nothing.)
        if (g_core3_wrote_pending) {
            file_remove(Root::Data, kCore3Pending);
            g_core3_wrote_pending = false;
        }
        g_core3_state = usable ? 1 : 0;
    }
    return g_core3_state == 1;
}
int helper_core() { return core3_usable() ? 3 : 2; }
void core3_retry_set(int attempts) { g_core3_retries = attempts < 1 ? 1 : (attempts > 20 ? 20 : attempts); }

// Threads that were started for core 3: after a lost core (resume) or three late periods they are moved to core 2.
static const int kMaxHelpers = 12;
static Thread* g_helpers[kMaxHelpers];
static std::atomic<int> g_helpers_lock{0};
static void helpers_lock() { while (g_helpers_lock.exchange(1, std::memory_order_acquire)) sceKernelDelayThread(50); }
static void helpers_unlock() { g_helpers_lock.store(0, std::memory_order_release); }
static std::atomic<uint32_t> g_c3_late{0}, g_c3_moved{0};
static bool g_c3_watch = true;
Core3Stats core3_stats() {
    Core3Stats c;
    c.probe_tries = g_core3_tries;
    c.late_periods = g_c3_late.load();
    c.threads_moved = g_c3_moved.load();
    return c;
}
void core3_watch_set(bool on) { g_c3_watch = on; }
bool core3_watch_on() { return g_c3_watch; }

bool core3_period(const char* who, uint64_t gap_us, uint64_t period_us) {
    // thread_local would be one more thing to get wrong on this C library: the caller (the pump) is the only user per name
    static LateCounter counter;
    if (!g_c3_watch || sceKernelGetCpuId() != 3) { counter.in_a_row = 0; return false; }
    const bool late = period_is_late(gap_us, period_us, 3, 1500000);
    if (late) {
        const uint32_t n = ++g_c3_late;
        if (n <= 20 || n % 100 == 0)
            logf("core3 watchdog: %s missed its deadline: a period of %u us took %u us (late period %u, %d in a row)", who, (unsigned)period_us,
                 (unsigned)gap_us, (unsigned)n, counter.in_a_row + 1);
    }
    const bool move = counter.note(late, 3);
    if (move) {
        counter.in_a_row = 0;
        g_c3_moved++;
        logf("core3 watchdog: %s moves to core 2 (three late periods in a row)", who);
    }
    return move;
}

static int core_mask(int core) {
    if (core == 3) return core3_usable() ? kCpu3Mask : SCE_KERNEL_CPU_MASK_USER_2;
    return (core >= 0 && core <= 2) ? (SCE_KERNEL_CPU_MASK_USER_0 << core) : SCE_KERNEL_CPU_MASK_USER_ALL;
}

Kind kind() { return Kind::Vita; }
const char* name() { return "vita"; }

uint64_t now_us() { return sceKernelGetProcessTimeWide(); }
void sleep_us(uint32_t us) { sceKernelDelayThread(us); }
void yield() { sceKernelDelayThread(1); }  // UNVERIFIED: whether DelayThread(0) yields on Vita; 1 us is the safe spelling

// ------------------------------------------------------------------------------------------------ logging
namespace {
SceUID g_log_fd = -1;
bool g_log_sync = false;
char g_log_path[512] = "";
SceKernelLwMutexWork g_log_mtx;
bool g_log_mtx_ok = false;
uint64_t g_t0 = 0;
}  // namespace

static void log_lock() { if (g_log_mtx_ok) sceKernelLockLwMutex(&g_log_mtx, 1, nullptr); }
static void log_unlock() { if (g_log_mtx_ok) sceKernelUnlockLwMutex(&g_log_mtx, 1); }

// Push the log to the file. sceIoSyncByFd is a no-op in Vita3K (unimplemented import) and the emulator keeps written data in a host
// buffer until the descriptor is closed, so the flush is "close and reopen for append", which is also correct on the device
// (one sceIoClose + sceIoOpen is sub-millisecond; it is only done once per [stat] line unless log_sync is set).
static void log_flush_locked() {
    if (g_log_fd < 0) return;
    sceIoSyncByFd(g_log_fd, 0);
    if (g_log_path[0]) {
        sceIoClose(g_log_fd);
        g_log_fd = sceIoOpen(g_log_path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    }
}

// Writes straight to the file under the log lock (the synchronous path: before the writer thread starts, in log_sync mode, and
// for what the writer takes from the queue).
static void log_write_direct(const char* text, size_t len) {
    log_lock();
    if (g_log_fd >= 0) {
        sceIoWrite(g_log_fd, text, len);
        if (g_log_sync) log_flush_locked();
    }
    log_unlock();
}

static void log_mirror(const char* text, size_t len) {
    // sceClibPrintf has a small internal buffer; the file is the authoritative record
    char tmp[256];
    size_t n = len < sizeof(tmp) - 1 ? len : sizeof(tmp) - 1;
    memcpy(tmp, text, n);
    tmp[n] = 0;
    sceClibPrintf("%s", tmp);
}

// ---- the queue and its writer thread
static LogQueue g_logq;
static std::atomic<bool> g_log_async{false};
static std::atomic<bool> g_log_writer_run{false};
static std::atomic<uint32_t> g_log_max_depth{0};
static Thread* g_log_writer = nullptr;
static SceKernelLwMutexWork g_drain_mtx;   // one consumer at a time: the writer thread, or a caller of log_flush / log_async_stop
static bool g_drain_mtx_ok = false;

// Takes what is queued and writes it to the file in as few calls as it can. Returns the lines taken.
static uint32_t log_drain() {
    if (!g_logq.ready()) return 0;
    static char stage[16 * 1024];
    static char line[LogQueue::kSlotBytes];
    if (g_drain_mtx_ok) sceKernelLockLwMutex(&g_drain_mtx, 1, nullptr);
    const uint32_t depth = g_logq.depth();
    if (depth > g_log_max_depth.load(std::memory_order_relaxed)) g_log_max_depth.store(depth, std::memory_order_relaxed);
    uint32_t taken = 0;
    size_t used = 0;
    size_t len = 0;
    while (g_logq.pop(line, &len)) {
        if (used + len > sizeof(stage)) {
            log_write_direct(stage, used);
            used = 0;
        }
        memcpy(stage + used, line, len);
        used += len;
        log_mirror(line, len);
        taken++;
    }
    const uint64_t lost = g_logq.take_dropped();
    if (lost) {
        char note[96];
        const int n = snprintf(note, sizeof(note), "[log] %llu line(s) dropped: the log queue was full\n", (unsigned long long)lost);
        if (used + (size_t)n > sizeof(stage)) { log_write_direct(stage, used); used = 0; }
        memcpy(stage + used, note, (size_t)n);
        used += (size_t)n;
    }
    if (used) log_write_direct(stage, used);
    if (g_drain_mtx_ok) sceKernelUnlockLwMutex(&g_drain_mtx, 1);
    return taken;
}

static void log_writer_main(void*) {
    while (g_log_writer_run.load(std::memory_order_acquire)) {
        if (log_drain() == 0) sceKernelDelayThread(20000);
        else sceKernelDelayThread(2000);   // let more lines gather: one write for many
    }
}

bool log_async_start(int core) {
    if (g_log_async.load() || g_log_sync) return g_log_async.load();
    if (!g_log_mtx_ok) return false;
    if (!g_drain_mtx_ok && sceKernelCreateLwMutex(&g_drain_mtx, "plat-log-drain", 0, 0, nullptr) >= 0) g_drain_mtx_ok = true;
    if (!g_logq.ready() && !g_logq.init(256)) {
        logf("log: no memory for the log queue, lines are written by their callers");
        return false;
    }
    g_log_writer_run = true;
    ThreadDesc d;
    d.name = "plat-log";
    d.fn = log_writer_main;
    d.core = core == 0 ? 2 : core;   // never core 0: that is the guest
    d.priority = 28;                 // the least urgent of the platform's threads
    d.stack_bytes = 32 * 1024;
    g_log_writer = thread_start(d);
    if (!g_log_writer) {
        g_log_writer_run = false;
        logf("log: the log writer thread did not start, lines are written by their callers");
        return false;
    }
    g_log_async = true;
    logf("log: asynchronous, a queue of 256 lines drained by a writer thread on core %d (config.txt log_async=0 writes in the caller)", d.core);
    return true;
}

void log_async_stop() {
    if (!g_log_async.exchange(false)) return;
    g_log_writer_run = false;
    thread_join(g_log_writer);
    g_log_writer = nullptr;
    log_drain();
}

LogStats log_stats() {
    LogStats st;
    st.async = g_log_async.load();
    st.queued = g_logq.pushed();
    st.dropped = g_logq.dropped();
    st.max_depth = g_log_max_depth.load(std::memory_order_relaxed);
    return st;
}

void log_raw(const char* text, size_t len) {
    if (!len) return;
    if (g_log_async.load(std::memory_order_acquire) && !g_log_sync) {
        g_logq.push(text, len);   // false: full, counted, reported by the writer
        return;
    }
    log_write_direct(text, len);
    log_mirror(text, len);
}

void logf(const char* fmt, ...) {
    char line[1024];
    uint64_t t = now_us() - g_t0;
    int off = snprintf(line, sizeof(line), "[%4u.%03u c%d] ", (unsigned)(t / 1000000u), (unsigned)((t / 1000u) % 1000u),
                       (int)sceKernelGetCpuId());
    if (off < 0) off = 0;
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
    if (g_log_async.load()) log_drain();
    log_lock();
    log_flush_locked();
    log_unlock();
}
void log_set_sync(bool on) {
    if (on && g_log_async.load()) log_drain();   // what is queued goes first; later lines are written in order by their callers
    g_log_sync = on;
}
const char* log_path() { return g_log_path; }

// Referenced by the debug vitaGL (built with -DsceClibPrintf=vgl_hook_printf): every vitaGL / vitaShaRK message ends up in the log.
extern "C" int vgl_hook_printf(const char* fmt, ...) {
    // GL errors ("file:line: function set GL_INVALID_...") can come once per draw: the first 64 are logged, then one line
    // per 4096 more, so a profile build with LOG_ERRORS still runs at speed.
    if (fmt != nullptr && fmt[0] == '%' && fmt[1] == 's' && fmt[2] == ':' && fmt[3] == '%' && fmt[4] == 'd') {
        static std::atomic<unsigned> errors{0};
        const unsigned seen = errors.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (seen > 64u) {
            if (seen % 4096u == 0u) plat::logf("[vgl] %u GL error messages so far (only the first 64 are logged)", seen);
            return 0;
        }
    }
    // DEBUG_GLSL_TRANSLATOR prints whole shader sources in one call (tens of KiB): probe the size, use the heap when needed.
    char small[1024];
    char* buf = small;
    size_t cap = sizeof(small);
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(small, sizeof(small), fmt, ap);
    va_end(ap);
    if (n >= (int)sizeof(small)) {
        char* big = (char*)malloc((size_t)n + 1);
        if (big) {
            buf = big;
            cap = (size_t)n + 1;
            vsnprintf(big, cap, fmt, ap2);
        } else {
            n = (int)sizeof(small) - 1;   // out of memory: keep the truncated head
        }
    }
    va_end(ap2);
    if (n < 0) { if (buf != small) free(buf); return n; }
    if ((size_t)n > cap - 1) n = (int)(cap - 1);
    // vitaGL messages already end in '\n'
    plat::log_raw("[vgl] ", 6);
    plat::log_raw(buf, (size_t)n);
    if (buf != small) free(buf);
    return n;
}

// ------------------------------------------------------------------------------------------------ paths and directories
bool path_resolve(Root r, const char* rel, char* out, size_t out_size) {
    const char* base = (r == Root::Data) ? kDataRoot : kAppRoot;
    if (!rel) rel = "";
    while (*rel == '/') rel++;
    int n = snprintf(out, out_size, "%s%s", base, rel);
    return n >= 0 && (size_t)n < out_size;
}

static bool mkdir_full(const char* full) {
    // full = "dev:dir/dir/dir" ; create each component after the "dev:" prefix. Existing directories are fine.
    char tmp[256];
    size_t len = strlen(full);
    if (len >= sizeof(tmp)) return false;
    memcpy(tmp, full, len + 1);
    while (len > 0 && tmp[len - 1] == '/') tmp[--len] = 0;
    char* start = strchr(tmp, ':');
    start = start ? start + 1 : tmp;
    for (char* p = start; *p; p++) {
        if (*p == '/' && p != start) {
            *p = 0;
            sceIoMkdir(tmp, 0777);
            *p = '/';
        }
    }
    int rc = sceIoMkdir(tmp, 0777);
    if (rc >= 0) return true;
    SceIoStat st;
    return sceIoGetstat(tmp, &st) >= 0 && SCE_S_ISDIR(st.st_mode);
}

bool dir_make(Root r, const char* rel) {
    char full[256];
    if (!path_resolve(r, rel, full, sizeof(full))) return false;
    return mkdir_full(full);
}

bool file_exists(Root r, const char* rel) {
    char full[256];
    SceIoStat st;
    return path_resolve(r, rel, full, sizeof(full)) && sceIoGetstat(full, &st) >= 0;
}

bool file_remove(Root r, const char* rel) {
    char full[256];
    return path_resolve(r, rel, full, sizeof(full)) && sceIoRemove(full) >= 0;
}

int64_t file_size_of(Root r, const char* rel) {
    char full[256];
    SceIoStat st;
    if (!path_resolve(r, rel, full, sizeof(full)) || sceIoGetstat(full, &st) < 0) return -1;
    return (int64_t)st.st_size;
}

// ------------------------------------------------------------------------------------------------ files
struct File {
    SceUID fd = -1;
    FileMode mode = FileMode::Read;
    int64_t phys = 0;       // OS file position
    int64_t size = 0;       // known size (Read mode), grows with writes
    uint8_t* buf = nullptr;
    size_t cap = 0;
    size_t buf_off = 0;     // Read: next unread byte in buf        Write: unused
    size_t buf_len = 0;     // Read: valid bytes in buf             Write: pending bytes
};

File* file_open(Root r, const char* rel, FileMode m, size_t buffer_bytes) {
    char full[256];
    if (!path_resolve(r, rel, full, sizeof(full))) return nullptr;
    int flags = SCE_O_RDONLY;
    if (m == FileMode::WriteTruncate) {
        // Vita3K does not honour SCE_O_TRUNC (a shorter rewrite keeps the old tail): remove first, so the file is really new.
        sceIoRemove(full);
        flags = SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC;
    }
    if (m == FileMode::Append) flags = SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND;
    SceUID fd = sceIoOpen(full, flags, 0777);
    if (fd < 0) return nullptr;
    File* f = new File();
    f->fd = fd;
    f->mode = m;
    if (m == FileMode::Read) {
        SceOff end = sceIoLseek(fd, 0, SCE_SEEK_END);
        sceIoLseek(fd, 0, SCE_SEEK_SET);
        f->size = end < 0 ? 0 : (int64_t)end;
    } else if (m == FileMode::Append) {
        SceOff end = sceIoLseek(fd, 0, SCE_SEEK_CUR);
        f->phys = f->size = end < 0 ? 0 : (int64_t)end;
    }
    if (buffer_bytes) {
        f->buf = (uint8_t*)memalign(64, buffer_bytes);
        if (f->buf) f->cap = buffer_bytes;
    }
    return f;
}

int64_t file_size(File* f) { return f->mode == FileMode::Read ? f->size : f->phys + (int64_t)f->buf_len; }

int64_t file_tell(File* f) {
    if (f->mode == FileMode::Read) return f->phys - (int64_t)(f->buf_len - f->buf_off);
    return f->phys + (int64_t)f->buf_len;
}

bool file_flush(File* f) {
    if (f->mode == FileMode::Read || f->buf_len == 0) return true;
    SceSSize w = sceIoWrite(f->fd, f->buf, f->buf_len);
    bool ok = w == (SceSSize)f->buf_len;
    f->phys += f->buf_len;
    f->size = f->phys;
    f->buf_len = 0;
    return ok;
}

bool file_seek(File* f, int64_t offset) {
    if (f->mode != FileMode::Read) {
        if (!file_flush(f)) return false;
        SceOff p = sceIoLseek(f->fd, offset, SCE_SEEK_SET);
        if (p < 0) return false;
        f->phys = (int64_t)p;
        return true;
    }
    int64_t buf_start = f->phys - (int64_t)f->buf_len;
    if (f->buf_len && offset >= buf_start && offset <= f->phys) {  // inside the read-ahead window
        f->buf_off = (size_t)(offset - buf_start);
        return true;
    }
    SceOff p = sceIoLseek(f->fd, offset, SCE_SEEK_SET);
    if (p < 0) return false;
    f->phys = (int64_t)p;
    f->buf_off = f->buf_len = 0;
    return true;
}

size_t file_read(File* f, void* dst, size_t n) {
    uint8_t* out = (uint8_t*)dst;
    size_t done = 0;
    while (done < n) {
        if (f->buf_off < f->buf_len) {
            size_t take = f->buf_len - f->buf_off;
            if (take > n - done) take = n - done;
            memcpy(out + done, f->buf + f->buf_off, take);
            f->buf_off += take;
            done += take;
            continue;
        }
        size_t want = n - done;
        if (!f->buf || want >= f->cap) {  // large request: straight into the destination
            SceSSize r = sceIoRead(f->fd, out + done, want);
            if (r <= 0) break;
            f->phys += r;
            done += (size_t)r;
            if ((size_t)r < want) break;
        } else {
            SceSSize r = sceIoRead(f->fd, f->buf, f->cap);
            if (r <= 0) break;
            f->phys += r;
            f->buf_off = 0;
            f->buf_len = (size_t)r;
        }
    }
    return done;
}

size_t file_write(File* f, const void* src, size_t n) {
    const uint8_t* in = (const uint8_t*)src;
    if (!f->buf) {
        SceSSize w = sceIoWrite(f->fd, in, n);
        if (w < 0) return 0;
        f->phys += w;
        f->size = f->phys;
        return (size_t)w;
    }
    size_t done = 0;
    while (done < n) {
        size_t space = f->cap - f->buf_len;
        size_t take = n - done < space ? n - done : space;
        memcpy(f->buf + f->buf_len, in + done, take);
        f->buf_len += take;
        done += take;
        if (f->buf_len == f->cap && !file_flush(f)) return done;
    }
    return done;
}

void file_close(File* f) {
    if (!f) return;
    file_flush(f);
    sceIoClose(f->fd);
    free(f->buf);
    delete f;
}

bool file_read_all(Root r, const char* rel, void** data, size_t* size) {
    File* f = file_open(r, rel, FileMode::Read);
    if (!f) return false;
    int64_t sz = f->size;
    void* p = malloc(sz ? (size_t)sz : 1);
    if (!p) { file_close(f); return false; }
    size_t got = file_read(f, p, (size_t)sz);
    file_close(f);
    if (got != (size_t)sz) { free(p); return false; }
    *data = p;
    *size = (size_t)sz;
    return true;
}

bool file_write_all(Root r, const char* rel, const void* data, size_t size) {
    File* f = file_open(r, rel, FileMode::WriteTruncate);
    if (!f) return false;
    size_t w = file_write(f, data, size);
    file_close(f);
    return w == size;
}

static int copy_tree_rec(const char* src_full, const char* dst_full, bool overwrite) {
    SceUID d = sceIoDopen(src_full);
    if (d < 0) return -1;
    mkdir_full(dst_full);
    int copied = 0;
    SceIoDirent ent;
    memset(&ent, 0, sizeof(ent));
    while (sceIoDread(d, &ent) > 0) {
        if (!strcmp(ent.d_name, ".") || !strcmp(ent.d_name, "..")) { memset(&ent, 0, sizeof(ent)); continue; }
        char s[256], t[256];
        snprintf(s, sizeof(s), "%s/%s", src_full, ent.d_name);
        snprintf(t, sizeof(t), "%s/%s", dst_full, ent.d_name);
        if (SCE_S_ISDIR(ent.d_stat.st_mode)) {
            int c = copy_tree_rec(s, t, overwrite);
            if (c > 0) copied += c;
        } else {
            SceIoStat st;
            if (!overwrite && sceIoGetstat(t, &st) >= 0) { memset(&ent, 0, sizeof(ent)); continue; }
            SceUID in = sceIoOpen(s, SCE_O_RDONLY, 0);
            if (in >= 0) sceIoRemove(t);   // see file_open: do not rely on SCE_O_TRUNC
            SceUID out = in >= 0 ? sceIoOpen(t, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777) : -1;
            if (in >= 0 && out >= 0) {
                static uint8_t chunk[64 * 1024];  // single-threaded installer, static to keep the stack small
                SceSSize n;
                bool ok = true;
                while ((n = sceIoRead(in, chunk, sizeof(chunk))) > 0) {
                    if (sceIoWrite(out, chunk, (SceSize)n) != n) { ok = false; break; }
                }
                if (ok) copied++;
            }
            if (in >= 0) sceIoClose(in);
            if (out >= 0) sceIoClose(out);
        }
        memset(&ent, 0, sizeof(ent));
    }
    sceIoDclose(d);
    return copied;
}

int dir_copy_tree(const char* src_rel_app, const char* dst_rel_data, bool overwrite) {
    char s[256], t[256];
    if (!path_resolve(Root::App, src_rel_app, s, sizeof(s)) || !path_resolve(Root::Data, dst_rel_data, t, sizeof(t))) return -1;
    size_t n = strlen(s);
    while (n > 0 && s[n - 1] == '/') s[--n] = 0;
    n = strlen(t);
    while (n > 0 && t[n - 1] == '/') t[--n] = 0;
    return copy_tree_rec(s, t, overwrite);
}

// ------------------------------------------------------------------------------------------------ clocks
ClockReport clocks_read() {
    ClockReport c;
    c.arm = scePowerGetArmClockFrequency();
    c.bus = scePowerGetBusClockFrequency();
    c.gpu = scePowerGetGpuClockFrequency();
    c.xbar = scePowerGetGpuXbarClockFrequency();
    return c;
}

int clock_measure_mhz() {
    // subs+bne on the Cortex-A9 is one cycle an iteration (the taken branch is folded), so iterations per microsecond are MHz.
    // Another thread on this core can only make a run slower: the best of three.
    int best = 0;
    for (int run = 0; run < 3; ++run) {
        uint32_t n = 2000000;
        const uint64_t t0 = now_us();
        asm volatile("1: subs %0, %0, #1\n\tbne 1b" : "+r"(n) : : "cc");
        const uint64_t dt = now_us() - t0;
        if (dt == 0) continue;
        const int mhz = (int)((2000000ull + dt / 2) / dt);
        if (mhz > best) best = mhz;
    }
    return best;
}

ClockReport clocks_set_max() {
    ClockReport c;
    c.req_arm = 444; c.req_bus = 222; c.req_gpu = 222; c.req_xbar = 166;
    c.rc_arm = scePowerSetArmClockFrequency(c.req_arm);
    c.rc_bus = scePowerSetBusClockFrequency(c.req_bus);
    c.rc_gpu = scePowerSetGpuClockFrequency(c.req_gpu);
    c.rc_xbar = scePowerSetGpuXbarClockFrequency(c.req_xbar);
    ClockReport r = clocks_read();
    c.arm = r.arm; c.bus = r.bus; c.gpu = r.gpu; c.xbar = r.xbar;
    logf("clocks: arm %d->%d (rc=0x%08X) bus %d->%d (rc=0x%08X) gpu %d->%d (rc=0x%08X) xbar %d->%d (rc=0x%08X)",
         c.req_arm, c.arm, (unsigned)c.rc_arm, c.req_bus, c.bus, (unsigned)c.rc_bus, c.req_gpu, c.gpu, (unsigned)c.rc_gpu,
         c.req_xbar, c.xbar, (unsigned)c.rc_xbar);
    logf("clocks: the ARM core measures %d MHz (the system reports %d)", clock_measure_mhz(), c.arm);
    return c;
}

// ------------------------------------------------------------------------------------------------ memory
bool mem_info(MemInfo* out) {
    SceKernelFreeMemorySizeInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    int rc = sceKernelGetFreeMemorySize(&info);
    if (rc >= 0) {
        out->user_free = info.size_user;
        out->cdram_free = info.size_cdram;
        out->phycont_free = info.size_phycont;
    }
    struct mallinfo mi = mallinfo();
    out->malloc_arena = mi.arena;
    out->malloc_used = mi.uordblks;
    out->malloc_free = mi.fordblks;
    return rc >= 0;
}

bool mem_alloc_block(const char* name, size_t size, size_t align, MemBlock* out) {
    *out = MemBlock();
    const size_t kPage = 4096, kMiB = 1024 * 1024;
    size_t rounded = size >= kMiB ? ((size + kMiB - 1) / kMiB) * kMiB : ((size + kPage - 1) / kPage) * kPage;
    if (align < kPage) align = kPage;

    // First try: ask the kernel for the alignment directly (UNVERIFIED that USER_RW honours > 4 KiB alignments on hardware;
    // Vita3K honours SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT).
    SceKernelAllocMemBlockOpt opt;
    memset(&opt, 0, sizeof(opt));
    opt.size = sizeof(opt);
    opt.attr = SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT;
    opt.alignment = (SceSize)align;
    SceUID uid = sceKernelAllocMemBlock(name, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, (SceSize)rounded, align > kPage ? &opt : nullptr);
    void* base = nullptr;
    if (uid >= 0) sceKernelGetMemBlockBase(uid, &base);
    if (uid >= 0 && base && (((uintptr_t)base) & (align - 1)) == 0) {
        out->base = out->raw_base = base;
        out->size = out->raw_size = rounded;
        out->uid = uid;
        logf("mem_alloc_block '%s': %u KiB at %p (uid 0x%X, kernel-aligned %u KiB)", name, (unsigned)(rounded / 1024), base, (unsigned)uid,
             (unsigned)(align / 1024));
        return true;
    }
    // Fallback: over-allocate and align by hand.
    if (uid >= 0) sceKernelFreeMemBlock(uid);
    size_t big = rounded + align;
    big = ((big + kMiB - 1) / kMiB) * kMiB;
    uid = sceKernelAllocMemBlock(name, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, (SceSize)big, nullptr);
    if (uid < 0) {
        logf("mem_alloc_block '%s': %u KiB FAILED rc=0x%08X", name, (unsigned)(big / 1024), (unsigned)uid);
        return false;
    }
    base = nullptr;
    sceKernelGetMemBlockBase(uid, &base);
    uintptr_t a = ((uintptr_t)base + align - 1) & ~(uintptr_t)(align - 1);
    out->raw_base = base;
    out->raw_size = big;
    out->base = (void*)a;
    out->size = rounded;
    out->uid = uid;
    logf("mem_alloc_block '%s': %u KiB raw %p aligned %p (manual %u KiB alignment, uid 0x%X)", name, (unsigned)(big / 1024), base,
         out->base, (unsigned)(align / 1024), (unsigned)uid);
    return true;
}

void mem_free_block(MemBlock* b) {
    if (b && b->uid >= 0) sceKernelFreeMemBlock(b->uid);
    if (b) *b = MemBlock();
}

uint64_t mem_touch_pages(void* base, size_t size) {
    uint64_t t0 = now_us();
    volatile uint8_t* p = (volatile uint8_t*)base;
    for (size_t i = 0; i < size; i += 4096) p[i] = p[i];  // read-modify-write keeps content, faults the page in
    return now_us() - t0;
}

// ------------------------------------------------------------------------------------------------ threads
struct Thread {
    SceUID thid = -1;
    ThreadFn fn = nullptr;
    void* arg = nullptr;
};

static int thread_tramp(SceSize args, void* argp) {
    Thread* t = args >= sizeof(Thread*) ? *(Thread**)argp : nullptr;
    if (t && t->fn) t->fn(t->arg);
    return 0;
}

Thread* thread_start(const ThreadDesc& d) {
    Thread* t = new Thread();
    t->fn = d.fn;
    t->arg = d.arg;
    size_t stack = d.stack_bytes < 0x4000 ? 0x4000 : ((d.stack_bytes + 0xFFF) & ~(size_t)0xFFF);
    t->thid = sceKernelCreateThread(d.name, thread_tramp, prio_abs(d.priority), (SceSize)stack, 0, core_mask(d.core), nullptr);
    if (t->thid < 0) {
        logf("thread_start '%s': sceKernelCreateThread failed 0x%08X (stack %u KiB)", d.name, (unsigned)t->thid, (unsigned)(stack / 1024));
        delete t;
        return nullptr;
    }
    int rc = sceKernelStartThread(t->thid, sizeof(Thread*), &t);
    if (rc < 0) {
        logf("thread_start '%s': sceKernelStartThread failed 0x%08X", d.name, (unsigned)rc);
        sceKernelDeleteThread(t->thid);
        delete t;
        return nullptr;
    }
    if (d.core == 3 && core3_usable()) {
        helpers_lock();
        for (int i = 0; i < kMaxHelpers; i++)
            if (!g_helpers[i]) { g_helpers[i] = t; break; }
        helpers_unlock();
    }
    return t;
}

void thread_join(Thread* t) {
    if (!t) return;
    helpers_lock();
    for (int i = 0; i < kMaxHelpers; i++)
        if (g_helpers[i] == t) g_helpers[i] = nullptr;
    helpers_unlock();
    int stat = 0;
    sceKernelWaitThreadEnd(t->thid, &stat, nullptr);
    sceKernelDeleteThread(t->thid);
    delete t;
}

int thread_current_id() { return (int)sceKernelGetThreadId(); }
int thread_current_core() { return (int)sceKernelGetCpuId(); }

bool thread_set_core(Thread* t, int core) {
    SceUID id = t ? t->thid : sceKernelGetThreadId();
    return sceKernelChangeThreadCpuAffinityMask(id, core_mask(core)) >= 0;
}

bool core3_reprobe() {
    if (!g_core3_allowed) return false;
    const bool was = g_core3_state == 1;
    if (g_core3_state < 0) return core3_usable();
    const bool now = core3_probe();
    if (was && !now) {
        g_core3_state = 0;   // core_mask(3) now answers core 2
        int moved = 0;
        helpers_lock();
        for (int i = 0; i < kMaxHelpers; i++)
            if (g_helpers[i] && thread_set_core(g_helpers[i], 3)) moved++;
        helpers_unlock();
        g_c3_moved += (uint32_t)moved;
        logf("core3: after the resume the fourth core is gone: %d helper thread(s) moved to core 2", moved);
    } else if (!was && now) {
        g_core3_state = 1;
        logf("core3: after the resume the fourth core is usable (threads already running stay where they are)");
    } else {
        logf("core3: after the resume the fourth core is %s, helper threads stay valid", now ? "still usable" : "still unavailable");
    }
    return g_core3_state == 1;
}

bool thread_set_priority(Thread* t, int priority) {
    SceUID id = t ? t->thid : sceKernelGetThreadId();   // the SDK doc example passes sceKernelGetThreadId() for the calling thread
    return sceKernelChangeThreadPriority(id, prio_abs(priority)) >= 0;
}

struct Mutex {
    SceKernelLwMutexWork work;
};
Mutex* mutex_create(const char* name) {
    Mutex* m = (Mutex*)memalign(8, sizeof(Mutex));
    memset(m, 0, sizeof(*m));
    if (sceKernelCreateLwMutex(&m->work, name ? name : "plat-mutex", 0, 0, nullptr) < 0) { free(m); return nullptr; }
    return m;
}
void mutex_lock(Mutex* m) { sceKernelLockLwMutex(&m->work, 1, nullptr); }
void mutex_unlock(Mutex* m) { sceKernelUnlockLwMutex(&m->work, 1); }
void mutex_destroy(Mutex* m) {
    if (!m) return;
    sceKernelDeleteLwMutex(&m->work);
    free(m);
}

struct Event {
    SceUID evid = -1;
    bool auto_reset = true;
};
Event* event_create(bool auto_reset, bool initially_signalled) {
    Event* e = new Event();
    e->auto_reset = auto_reset;
    e->evid = sceKernelCreateEventFlag("plat-event", SCE_EVENT_WAITMULTIPLE, initially_signalled ? 1 : 0, nullptr);
    if (e->evid < 0) { delete e; return nullptr; }
    return e;
}
void event_signal(Event* e) { sceKernelSetEventFlag(e->evid, 1); }
void event_reset(Event* e) { sceKernelClearEventFlag(e->evid, ~1u); }  // the argument is the pattern that is ANDed (PSP/Vita semantics)
bool event_wait(Event* e, int64_t timeout_us) {
    unsigned int out = 0;
    unsigned int mode = SCE_EVENT_WAITOR | (e->auto_reset ? SCE_EVENT_WAITCLEAR : 0);
    int rc;
    if (timeout_us == 0) {
        rc = sceKernelPollEventFlag(e->evid, 1, mode, &out);
    } else if (timeout_us < 0) {
        rc = sceKernelWaitEventFlag(e->evid, 1, mode, &out, nullptr);
    } else {
        SceUInt to = timeout_us > 0x7FFFFFFF ? 0x7FFFFFFF : (SceUInt)timeout_us;
        rc = sceKernelWaitEventFlag(e->evid, 1, mode, &out, &to);
    }
    return rc >= 0;
}
void event_destroy(Event* e) {
    if (!e) return;
    sceKernelDeleteEventFlag(e->evid);
    delete e;
}

// ------------------------------------------------------------------------------------------------ pad
bool pad_poll(PadState* out) {
    *out = PadState();
    SceCtrlData d;
    memset(&d, 0, sizeof(d));
    int n = sceCtrlPeekBufferPositive2(0, &d, 1);
    out->time_us = now_us();
    if (n < 1) return false;
    out->raw_buttons = d.buttons;
    uint32_t b = d.buttons & (PAD_SELECT | PAD_START | PAD_UP | PAD_RIGHT | PAD_DOWN | PAD_LEFT | PAD_LTRIGGER | PAD_RTRIGGER |
                              PAD_TRIANGLE | PAD_CIRCLE | PAD_CROSS | PAD_SQUARE);
    if (d.buttons & SCE_CTRL_L1) b |= PAD_LTRIGGER;
    if (d.buttons & SCE_CTRL_R1) b |= PAD_RTRIGGER;
    if (d.buttons & SCE_CTRL_PSBUTTON) b |= PAD_HOME;
    out->buttons = b;
    auto dz = [](uint8_t v) -> uint8_t { return (v > 120 && v < 136) ? 128 : v; };
    out->lx = dz(d.lx);
    out->ly = dz(d.ly);
    out->rx = dz(d.rx);
    out->ry = dz(d.ry);
    return true;
}

// Touch screen: the top-right corner is a button (the settings menu). The panel reports 1920 x 1088.
bool touch_corner_tap() {
    static bool started = false;
    static bool down_before = false;
    if (!started) {
        started = true;
        sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    }
    SceTouchData t;
    memset(&t, 0, sizeof(t));
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &t, 1) < 1) return false;
    bool in_corner = false;
    for (unsigned i = 0; i < t.reportNum && i < SCE_TOUCH_MAX_REPORT; ++i)
        if (t.report[i].x >= 1920 - 310 && t.report[i].y < 220) in_corner = true;
    const bool tap = in_corner && !down_before;
    down_before = in_corner;
    return tap;
}

// ---- 500 MHz through PSVshellPlus: a profile for this title that the game keeps in place (vita/psvshell/INFC00001 is the 500 variant).
// PSVSv0300 layout: "PSVS0300", lock flags 0x0b, then CPU, GPU, GPU-crossbar and bus clocks as little-endian words.
static const unsigned char kProfile500[28] = {'P', 'S', 'V', 'S', '0', '3', '0', '0', 0x0b, 0, 0, 0, 0xf4, 0x01, 0, 0, 0xde, 0, 0, 0, 0xde, 0, 0, 0, 0xa6, 0, 0, 0};
static const char kProfileDir[] = "ur0:data/PSVshell/profiles";
static const char kProfilePath[] = "ur0:data/PSVshell/profiles/INFC00001";
static const char kProfileTmp[] = "ur0:data/PSVshell/profiles/INFC00001.tmp";

bool psvshell_installed() {
    SceIoStat st;
    return sceIoGetstat("ur0:tai/PSVshellPlus_Kernel.skprx", &st) >= 0 || sceIoGetstat(kProfileDir, &st) >= 0;
}

static bool io_rename(const char* a, const char* b) { return sceIoRename(a, b) >= 0; }
static bool io_remove(const char* p) { return sceIoRemove(p) >= 0; }
static bool io_exists(const char* p) { SceIoStat st; return sceIoGetstat(p, &st) >= 0; }
static const filereplace::Ops& sceio_ops() {
    static const filereplace::Ops ops{&io_rename, &io_remove, &io_exists};
    return ops;
}

int psvshell_profile_set(int mhz) {
    // A run killed between the two renames left INFC00001.bak and no profile: the old one comes back first.
    if (filereplace::recover(sceio_ops(), kProfilePath)) logf("clock: the PSVshellPlus profile was restored from its .bak");
    unsigned char want[28], v444[28];
    memcpy(want, kProfile500, sizeof(want));
    memcpy(v444, kProfile500, sizeof(v444));
    v444[12] = 0xbc; v444[13] = 0x01;                 // 444 = 0x1bc
    if (mhz <= 444) memcpy(want, v444, sizeof(want));
    unsigned char have[40];
    int have_len = -1;
    const SceUID in = sceIoOpen(kProfilePath, SCE_O_RDONLY, 0);
    if (in >= 0) {
        have_len = sceIoRead(in, have, sizeof(have));
        sceIoClose(in);
    }
    if (in >= 0) {
        if (have_len == 28 && memcmp(have, want, 28) == 0) return 0;
        if (have_len != 28 || (memcmp(have, kProfile500, 28) != 0 && memcmp(have, v444, 28) != 0)) {
            logf("clock: %s is a profile of your own (not one of the game's two): left as it is", kProfilePath);
            return -1;
        }
    } else if (mhz <= 444) {
        return 0;                                       // 444 never creates a profile
    }
    SceIoStat st;
    if (sceIoGetstat("ur0:data/PSVshell", &st) < 0) sceIoMkdir("ur0:data/PSVshell", 0777);
    if (sceIoGetstat(kProfileDir, &st) < 0) sceIoMkdir(kProfileDir, 0777);
    const SceUID out = sceIoOpen(kProfileTmp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (out < 0) {
        logf("clock: cannot write %s (0x%08X)", kProfileTmp, (unsigned)out);
        return -2;
    }
    const int wrote = sceIoWrite(out, want, sizeof(want));
    sceIoClose(out);
    if (wrote != (int)sizeof(want)) {
        logf("clock: short write to %s (%d)", kProfileTmp, wrote);
        sceIoRemove(kProfileTmp);
        return -2;
    }
    // sceIoRename may refuse an existing target: filereplace moves the old profile to .bak first (and never leaves a moment with none)
    if (filereplace::replace(sceio_ops(), kProfileTmp, kProfilePath) == filereplace::Result::Failed) {
        logf("clock: cannot rename the profile into place");
        sceIoRemove(kProfileTmp);
        return -2;
    }
    logf("clock: PSVshellPlus profile %s written (%d MHz)", kProfilePath, mhz <= 444 ? 444 : 500);
    return 1;
}

static int g_arm_requested = 0;   // the last ARM clock the application asked for: put back after a resume
int clock_set_arm(int mhz, int* applied_mhz) {
    g_arm_requested = mhz;
    const int rc = scePowerSetArmClockFrequency(mhz);
    const int now = scePowerGetArmClockFrequency();
    if (applied_mhz) *applied_mhz = now;
    logf("clock: ARM %d MHz requested, rc=0x%08X, reads back %d MHz", mhz, (unsigned)rc, now);
    return rc;
}

// ------------------------------------------------------------------------------------------------ power
static bool g_power_tick_on = true;
static std::atomic<uint64_t> g_power_ticks{0};
static std::atomic<uint64_t> g_power_last_us{0};
void power_tick_set(bool on) { g_power_tick_on = on; }
uint64_t power_tick_count() { return g_power_ticks.load(); }
void power_tick(uint32_t min_interval_ms) {
    if (!g_power_tick_on) return;
    const uint64_t now = now_us();
    const uint64_t last = g_power_last_us.load(std::memory_order_relaxed);
    if (last != 0 && now - last < (uint64_t)min_interval_ms * 1000u) return;
    g_power_last_us.store(now, std::memory_order_relaxed);
    sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);   // the console does not suspend by itself
    sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_OLED_OFF);       // the screen does not switch off
    sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_OLED_DIMMING);   // and does not dim first
    g_power_ticks++;
}

static const int kMaxResumeHandlers = 8;
static ResumeHandler g_resume_handlers[kMaxResumeHandlers];
static std::atomic<uint32_t> g_suspends{0}, g_resumes{0};
static std::atomic<int> g_mhz_after_resume{0};
static std::atomic<uint64_t> g_last_resume_us{0};
void resume_handler_add(ResumeHandler h) {
    for (int i = 0; i < kMaxResumeHandlers; i++)
        if (!g_resume_handlers[i]) { g_resume_handlers[i] = h; return; }
}
PowerStats power_stats() {
    PowerStats p;
    p.suspends = g_suspends.load();
    p.resumes = g_resumes.load();
    p.mhz_after_resume = g_mhz_after_resume.load();
    return p;
}
void audio_note_resume();   // audio_vita.cpp

static const unsigned kPowerSuspendBits = SCE_POWER_CB_SYSTEM_SUSPEND | SCE_POWER_CB_APP_SUSPEND;
static const unsigned kPowerResumeBits = SCE_POWER_CB_SYSTEM_RESUME | SCE_POWER_CB_SYSTEM_RESUMING | SCE_POWER_CB_APP_RESUME |
                                         SCE_POWER_CB_APP_RESUMING | SCE_POWER_CB_AFTER_SYSTEM_RESUME;

// What is done once after the console came back (the notification comes in several bits, one after the other: the first one counts).
static void do_resume() {
    const uint32_t n = ++g_resumes;
    sceKernelDelayThread(300000);   // let the system settle: the pad, the card and the audio device wake up
    logf("resume %u: the console is back; re-checking the clock, core 3, the audio port and the disc image", (unsigned)n);
    if (g_arm_requested > 0 && scePowerGetArmClockFrequency() != g_arm_requested) {
        const int rc = scePowerSetArmClockFrequency(g_arm_requested);
        logf("resume %u: the ARM clock was %d MHz, asked for %d again (rc=0x%08X), now %d MHz", (unsigned)n, scePowerGetArmClockFrequency(),
             g_arm_requested, (unsigned)rc, scePowerGetArmClockFrequency());
    }
    const int mhz = clock_measure_mhz();
    g_mhz_after_resume = mhz;
    logf("resume %u: the ARM core measures %d MHz (the system reports %d); core 3 %s", (unsigned)n, mhz, scePowerGetArmClockFrequency(),
         core3_reprobe() ? "usable" : "not in use");
    audio_note_resume();
    for (int i = 0; i < kMaxResumeHandlers; i++)
        if (g_resume_handlers[i]) g_resume_handlers[i]();
}

static int power_callback(int, int, int arg, void*) {
    const unsigned bits = (unsigned)arg;
    if (bits & kPowerSuspendBits) {
        const uint32_t n = ++g_suspends;
        logf("suspend %u: power callback 0x%08X", (unsigned)n, bits);
    }
    if (bits & kPowerResumeBits) {
        // SYSTEM_RESUMING, SYSTEM_RESUME and APP_RESUME arrive within a moment of each other: one resume.
        const uint64_t now = now_us();
        const uint64_t last = g_last_resume_us.load();
        if (last == 0 || now - last > 3000000u) {
            g_last_resume_us = now;
            logf("resume: power callback 0x%08X", bits);
            do_resume();
        }
    }
    return 0;
}

static int power_thread_main(SceSize, void*) {
    const SceUID cb = sceKernelCreateCallback("infected-power", 0, power_callback, nullptr);
    if (cb < 0) {
        logf("power: sceKernelCreateCallback failed 0x%08X: suspend and resume are not handled", (unsigned)cb);
        return 0;
    }
    const int rc = scePowerRegisterCallback(cb);
    logf("power: callback registered (rc=0x%08X): a resume re-checks the clock, core 3, the audio port and the disc image", (unsigned)rc);
    for (;;) sceKernelDelayThreadCB(250000);   // callbacks run inside a callback-aware wait of the thread that made them
    return 0;
}

void power_callbacks_start() {
    static bool started = false;
    if (started) return;
    started = true;
    const SceUID id = sceKernelCreateThread("plat-power", power_thread_main, prio_abs(20), 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_2, nullptr);
    if (id < 0 || sceKernelStartThread(id, 0, nullptr) < 0) logf("power: the callback thread did not start (0x%08X)", (unsigned)id);
}

// ------------------------------------------------------------------------------------------------ lifecycle
bool init(const InitParams& p) {
    g_t0 = now_us();
    if (sceKernelCreateLwMutex(&g_log_mtx, "plat-log", 0, 0, nullptr) >= 0) g_log_mtx_ok = true;
    if (sceKernelCreateLwMutex(&g_drain_mtx, "plat-log-drain", 0, 0, nullptr) >= 0) g_drain_mtx_ok = true;
    bool ok = dir_make(Root::Data, "");
    if (p.log_file) {
        char full[256];
        path_resolve(Root::Data, p.log_file, full, sizeof(full));
        snprintf(g_log_path, sizeof(g_log_path), "%s", full);
        sceIoRemove(full);   // Vita3K ignores SCE_O_TRUNC: start from a really empty file
        g_log_fd = sceIoOpen(full, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
        g_log_sync = p.log_sync_each_line;
    }
    logf("platform init: vita, data root %s, log %s (fd %d), flush_each_line=%d", kDataRoot, g_log_path[0] ? g_log_path : "(none)",
         (int)g_log_fd, (int)p.log_flush_each_line);
    if (!ok) logf("WARNING: could not create %s", kDataRoot);
    if (p.set_max_clocks) clocks_set_max();
    if (p.init_pad) sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    return ok;
}

void shutdown() {
    audio_close();
    core3_release();
    logf("platform shutdown");
    log_async_stop();   // the queue is written out before the file is closed
    log_lock();
    if (g_log_fd >= 0) { sceIoClose(g_log_fd); g_log_fd = -1; }
    log_unlock();
}

void exit_process(int code) {
    (void)code;
    sceKernelExitProcess(0);
    for (;;) {}
}

}  // namespace plat
