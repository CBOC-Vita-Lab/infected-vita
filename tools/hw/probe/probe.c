// Hardware probe: which calls does the Vita kernel accept while the stack pointer is outside the thread's own stack?
// Variant number comes from ux0:data/infected/probe.txt; every step is appended to ux0:data/infected/probe.log and closed at once.
#include <psp2/fiber.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/sysmodule.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>

int _newlib_heap_size_user = 16 * 1024 * 1024;

static void logf_(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    buf[n++] = '\n';
    SceUID fd = sceIoOpen("ux0:data/infected/probe.log", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (fd >= 0) { sceIoWrite(fd, buf, n); sceIoClose(fd); }
}
// The same without any call that leaves user mode: lines pile up here and are written from the thread's own stack.
static char g_quiet[4096];
static int g_quiet_n;
static void quietf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    g_quiet_n += vsnprintf(g_quiet + g_quiet_n, sizeof(g_quiet) - g_quiet_n - 2, fmt, ap);
    va_end(ap);
    g_quiet[g_quiet_n++] = '\n';
}
static void quiet_flush(void) { if (g_quiet_n) { g_quiet[g_quiet_n] = 0; logf_("%s", g_quiet); g_quiet_n = 0; } }

static inline uint32_t *tls(void) { uint32_t *p; __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(p)); return p; }
static inline uint32_t cur_sp(void) { uint32_t s; __asm__ volatile("mov %0, sp" : "=r"(s)); return s; }

void call_on_stack(void (*fn)(void *), void *arg, void *new_sp);
__asm__(".syntax unified\n.text\n.align 2\n.globl call_on_stack\n.type call_on_stack,%function\n.thumb\n.thumb_func\n"
        "call_on_stack:\n push {r4, lr}\n mov r4, sp\n mov sp, r2\n mov r2, r0\n mov r0, r1\n blx r2\n mov sp, r4\n pop {r4, pc}\n");

static volatile int g_rival_stop;
static int rival_main(SceSize n, void *p) { (void)n; (void)p; while (!g_rival_stop) sceKernelDelayThread(300); return 0; }
static SceUID rival_start(void) {
    SceUID th = sceKernelCreateThread("probe-rival", rival_main, 0x10000100 - 32, 0x10000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0);
    sceKernelStartThread(th, 0, NULL);
    return th;
}
static SceUID g_fd;
static int g_what;
static uint32_t g_snap[0x240];
static void snap_tls(void) { memcpy(g_snap, (uint8_t *)tls() - 0x800, sizeof(g_snap)); }
static void diff_tls(const char *tag) {
    const uint32_t *now = (const uint32_t *)((uint8_t *)tls() - 0x800);
    for (unsigned i = 0; i < 0x240; i++)
        if (now[i] != g_snap[i]) quietf("%s tls[%+d]: %08x -> %08x", tag, (int)(i * 4) - 0x800, (unsigned)g_snap[i], (unsigned)now[i]);
}
static void body(void *tag) {
    quietf("%s: on sp=%08x", (const char *)tag, (unsigned)cur_sp());
    if (g_what & 1) { SceUInt64 t = sceKernelGetProcessTimeWide(); quietf("%s: time ok %u", (const char *)tag, (unsigned)t); }
    if (g_what & 2) { int r = sceIoLseek32(g_fd, 0, SCE_SEEK_SET); quietf("%s: lseek32 ok %d", (const char *)tag, r); }
    if (g_what & 4) { char b[4]; int r = sceIoRead(g_fd, b, 4); quietf("%s: read ok %d", (const char *)tag, r); }
    if (g_what & 8) { int r = sceKernelDelayThread(2000); quietf("%s: delay ok %d", (const char *)tag, r); }
    if (g_what & 32) { SceUInt64 t0 = sceKernelGetProcessTimeWide(); volatile unsigned spin = 0; while (sceKernelGetProcessTimeWide() - t0 < 3000000) spin++; quietf("%s: 3 s spin ok while a higher-priority thread on the same core wakes every 0.3 ms", (const char *)tag); }
    if (g_what & 16) { SceUInt64 t0 = sceKernelGetProcessTimeWide(); while (sceKernelGetProcessTimeWide() - t0 < 300000) {} quietf("%s: 300 ms spin ok (pre-emption with this sp)", (const char *)tag); }
}
static void fiber_entry(SceUInt32 a, SceUInt32 b) {
    (void)a; (void)b;
    diff_tls("fiber");
    body("fiber");
    sceFiberReturnToThread(0, NULL);
}
static int thread_main(SceSize n, void *p) {
    (void)n;
    int variant = *(int *)p;
    SceKernelThreadInfo ti;
    memset(&ti, 0, sizeof(ti));
    ti.size = sizeof(ti);
    sceKernelGetThreadInfo(sceKernelGetThreadId(), &ti);
    logf_("variant %d: thread stack %08x..%08x sp=%08x tls=%08x", variant, (unsigned)(uintptr_t)ti.stack,
          (unsigned)((uintptr_t)ti.stack + ti.stackSize), (unsigned)cur_sp(), (unsigned)(uintptr_t)tls());
    const size_t size = 256 * 1024;
    uint8_t *heap_stack = memalign(4096, size);
    switch (variant) {
    case 0: {  // TLS words that look like this thread's stack bounds
        const uint32_t *w = (const uint32_t *)((uint8_t *)tls() - 0x800);
        char line[400]; int k = 0;
        for (unsigned i = 0; i < 0x240; i++)
            if (w[i]) { k += snprintf(line + k, sizeof(line) - k, " [%+d]=%08x", (int)(i * 4) - 0x800, (unsigned)w[i]); if (k > 300) { logf_("tls:%s", line); k = 0; } }
        if (k) logf_("tls:%s", line);
        break; }
    case 1: g_what = 1; call_on_stack(body, "heap", heap_stack + size); break;
    case 2: g_what = 2; call_on_stack(body, "heap", heap_stack + size); break;
    case 3: g_what = 4; call_on_stack(body, "heap", heap_stack + size); break;
    case 4: g_what = 8; call_on_stack(body, "heap", heap_stack + size); break;
    case 5: g_what = 16; call_on_stack(body, "heap", heap_stack + size); break;
    case 6: {
        void *base = NULL;
        SceUID blk = sceKernelAllocMemBlock("probe-stack", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 1024 * 1024, NULL);
        sceKernelGetMemBlockBase(blk, &base);
        g_what = 1 | 2 | 4 | 8;
        call_on_stack(body, "memblock", (uint8_t *)base + 1024 * 1024);
        break; }
    case 7: case 8: {
        int rc = sceSysmoduleLoadModule(SCE_SYSMODULE_FIBER);
        logf_("fiber module load rc=%08x", rc);
        static SceFiber fiber;
        snap_tls();
        rc = _sceFiberInitializeImpl(&fiber, "probe", fiber_entry, 0, heap_stack, size, NULL);
        logf_("fiber init rc=%08x", rc);
        g_what = variant == 7 ? 1 : (1 | 2 | 4 | 8 | 16);
        SceUInt32 back = 0;
        rc = sceFiberRun(&fiber, 0, &back);
        quiet_flush();
        logf_("fiber run rc=%08x", rc);
        diff_tls("after");
        break; }
    case 10: {  // pre-emption by another thread while on a foreign stack, bounds untouched
        rival_start(); g_what = 32; call_on_stack(body, "heap+rival", heap_stack + size); g_rival_stop = 1; break; }
    case 11: case 12: {  // 11: exact bounds, 12: wide-open bounds (0..0xFFFFFFFF); everything, with the rival
        uint32_t *w = (uint32_t *)((uint8_t *)tls() - 0x800);
        uint32_t keep_a = w[2], keep_b = w[3];
        rival_start();
        w[2] = variant == 11 ? (uint32_t)(uintptr_t)(heap_stack + size) : 0xFFFFFFFFu;
        w[3] = variant == 11 ? (uint32_t)(uintptr_t)heap_stack : 0u;
        g_what = 1 | 2 | 4 | 8 | 32;
        call_on_stack(body, variant == 11 ? "heap+exact+rival" : "heap+wide+rival", heap_stack + size);
        quiet_flush();
        if (variant == 12) { g_what = 2 | 4 | 8 | 32; body("own stack, wide bounds"); }
        w[2] = keep_a; w[3] = keep_b; g_rival_stop = 1;
        break; }
    case 9: {  // stack bounds written into the TLS words by hand (which ones: probe.txt second number, third = limit word)
        extern int g_arg[4];
        uint32_t *w = (uint32_t *)((uint8_t *)tls() - 0x800);
        uint32_t keep_a = w[g_arg[1]], keep_b = w[g_arg[2]];
        w[g_arg[1]] = (uint32_t)(uintptr_t)(heap_stack + size);
        w[g_arg[2]] = (uint32_t)(uintptr_t)heap_stack;
        g_what = 1 | 2 | 4 | 8;
        call_on_stack(body, "heap+tls", heap_stack + size);
        w[g_arg[1]] = keep_a; w[g_arg[2]] = keep_b;
        break; }
    }
    quiet_flush();
    logf_("variant %d: survived", variant);
    return 0;
}
int g_arg[4];
int main(void) {
    char text[64] = {0};
    SceUID fd = sceIoOpen("ux0:data/infected/probe.txt", SCE_O_RDONLY, 0);
    if (fd >= 0) { sceIoRead(fd, text, sizeof(text) - 1); sceIoClose(fd); }
    sscanf(text, "%d %d %d %d", &g_arg[0], &g_arg[1], &g_arg[2], &g_arg[3]);
    g_fd = sceIoOpen("ux0:data/infected/probe.txt", SCE_O_RDONLY, 0);
    SceUID th = sceKernelCreateThread("probe-thread", thread_main, 0x10000100, 0x100000, 0, 0, NULL);
    sceKernelStartThread(th, sizeof(int), &g_arg[0]);
    sceKernelWaitThreadEnd(th, NULL, NULL);
    logf_("variant %d: process exits normally", g_arg[0]);
    sceKernelExitProcess(0);
    return 0;
}
