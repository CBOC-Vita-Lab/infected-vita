// infected_clock.skprx: runs the PS Vita's ARM cores at 500 MHz for this game, which the system's own power API refuses (it takes
// 444 MHz at most; the user-mode call fails above that). The application loads it from its own package when the player chooses
// 500 MHz (taiLoadStartKernelModule, taiHEN is there on every HENkaku/Ensō console) and unloads it for 444 MHz and when it quits.
// Nothing is installed, and the module is not a plugin: it is never listed in config.txt.
//
// How: the ARM clock is a multiplier register of the SoC's "pervasive" block (physical 0xE3103000; 0xE3103004 is a divisor that
// stays 0 at full speed). The system sets it to the 444 MHz step (multiplier index 8, or 14 with the L2 cache at core speed); the
// next step of the same kind is 500 MHz (9, or 15), the hardware's own table (the henkaku.xyz wiki's Pervasive page lists it).
// The module maps that register, checks that it holds exactly one of those two 444 values, and writes the next index. Nothing else
// is touched, and the system's own clock bookkeeping is not (so scePowerGetArmClockFrequency keeps saying 444: the game measures
// the real clock with a timing loop). This is the idea of the kernel overclockers (PSVshell, LOLIcon); no code of theirs is used.
//
// Safety, in order of strictness:
//  - only firmware 3.60-3.74 (the real version, like infected_core3); anything else: nothing is mapped, nothing is written;
//  - the register must read exactly 444 MHz at full speed (index 8 or 14, divisor 0); any other value is left alone;
//  - a register that already reads 500 MHz belongs to someone else (a PSVshellPlus profile): nothing is written, and nothing is
//    restored later;
//  - the old value comes back when the module stops (the application unloads it for 444 MHz and at quit), AND when the process that
//    asked for 500 MHz exits or is killed (a crash), through a process event handler. The value is written back only if the register
//    still holds what this module wrote, so someone else's later change is never undone. If the handler cannot be registered the
//    module refuses to start (no 500 MHz without its safety net);
//  - the module writes what it did to ux0:data/infected/clock_module.txt; the application copies the lines into its own log.
#include <psp2kern/io/fcntl.h>
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/proc_event.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/sysroot.h>
#include <psp2kern/kernel/threadmgr.h>

#define BASECLK_PA 0xE3103000u            // ScePervasiveBaseClk: +0 ARM multiplier index, +4 ARM divisor
#define KERNEL_PID_VALUE 0x10005
#define LOG_PATH "ux0:data/infected/clock_module.txt"

static volatile unsigned int *g_reg;       // the mapped register page
static SceUID g_block = -1;
static SceUID g_handler = -1;
static SceUID g_owner = -1;                // the process that asked for 500 MHz
static unsigned int g_orig;                // the multiplier index to put back
static unsigned int g_set;                 // the one this module wrote
static volatile int g_engaged;             // 1 while this module's write is in the register

// ---- the log file (best effort) ---------------------------------------------------------------------------------------------
static char g_log[512];
static int g_log_len;
static int g_log_started;   // the first write of a start truncates the file, later ones append

static void put(const char *s) {
    while (*s && g_log_len < (int)sizeof(g_log) - 1) g_log[g_log_len++] = *s++;
}
static void put_hex(unsigned int v) {
    static const char digits[] = "0123456789abcdef";
    put("0x");
    for (int i = 28; i >= 0; i -= 4) {
        const char c = digits[(v >> i) & 0xF];
        if (g_log_len < (int)sizeof(g_log) - 1) g_log[g_log_len++] = c;
    }
}
static void put_dec(int v) {
    char tmp[12];
    int n = 0;
    if (v < 0) { put("-"); v = -v; }
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 11);
    while (n--) if (g_log_len < (int)sizeof(g_log) - 1) g_log[g_log_len++] = tmp[n];
}
static void flush_log(int append) {
    if (g_log_len == 0) return;
    if (!g_log_started) { g_log_started = 1; append = 0; }
    const SceUID fd = ksceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | (append ? SCE_O_APPEND : SCE_O_TRUNC), 0777);
    if (fd >= 0) {
        ksceIoWrite(fd, g_log, (SceSize)g_log_len);
        ksceIoClose(fd);
    }
    g_log_len = 0;
}

// No libc here: a plain byte loop (the build turns off the compiler turning it into a memset call).
static void zero(void *p, unsigned int n) {
    volatile unsigned char *b = (volatile unsigned char *)p;
    while (n--) *b++ = 0;
}

// ---- the register -----------------------------------------------------------------------------------------------------------
static void write_mult(unsigned int value) {
    g_reg[0] = value;
    __asm__ volatile("dsb" ::: "memory");
}

// Puts the old multiplier back if (and only if) the register still holds what this module wrote.
static void restore(const char *why) {
    if (!__sync_bool_compare_and_swap(&g_engaged, 1, 0)) return;
    const unsigned int now = g_reg[0];
    put("clock: ");
    put(why);
    if (now == g_set) {
        write_mult(g_orig);
        put(": multiplier "); put_dec((int)g_set); put(" -> "); put_dec((int)g_orig); put(" (restored), reads "); put_dec((int)g_reg[0]);
    } else {
        put(": the register reads "); put_dec((int)now); put(" now, not what this module wrote ("); put_dec((int)g_set); put("): left alone");
    }
    put("\n");
    flush_log(1);
}

static int on_exit(SceUID pid, SceProcEventInvokeParam1 *a2, int a3) {
    (void)a2; (void)a3;
    if (pid == g_owner) restore("owner process exited");
    return 0;
}
static int on_kill(SceUID pid, SceProcEventInvokeParam1 *a2, int a3) {
    (void)a2; (void)a3;
    if (pid == g_owner) restore("owner process killed");
    return 0;
}

// ---- start / stop -----------------------------------------------------------------------------------------------------------
static int arg_int(const char *s, const char *key, int def) {
    for (int at = 0; s[at]; ++at) {
        if (at != 0 && s[at - 1] != ' ') continue;
        int k = 0;
        while (key[k] && s[at + k] == key[k]) ++k;
        if (key[k] != '\0') continue;
        int v = 0, any = 0;
        for (const char *p = s + at + k; *p >= '0' && *p <= '9'; ++p) { v = v * 10 + (*p - '0'); any = 1; }
        if (any) return v;
    }
    return def;
}

static int fail(const char *why, int code) {
    put("clock: "); put(why); put(" (");  put_hex((unsigned int)code); put(")\n");
    if (g_handler >= 0) { ksceKernelUnregisterProcEventHandler(g_handler); g_handler = -1; }
    if (g_block >= 0) { ksceKernelFreeMemBlock(g_block); g_block = -1; }
    g_reg = 0;
    flush_log(1);
    return SCE_KERNEL_START_FAILED;
}

int _start(SceSize argc, const void *args) __attribute__((weak, alias("module_start")));
int module_start(SceSize argc, const void *args) {
    char text[64];
    unsigned int n = 0;
    if (args) for (; n < argc && n < sizeof(text) - 1 && ((const char *)args)[n]; ++n) text[n] = ((const char *)args)[n];
    text[n] = '\0';
    g_log_len = 0;
    g_log_started = 0;
    put("clock: module start, arguments \""); put(text); put("\"\n");

    const int firmware = ksceKernelSysrootGetSystemSwVersion();
    if (firmware < 0x03600000 || firmware >= 0x03750000) return fail("firmware not supported, nothing touched", firmware);
    const int target = arg_int(text, "arm=", 0);
    const int stale = arg_int(text, "stale=", 0);
    const int given = arg_int(text, "pid=", 0);
    if (target != 500 || given <= 0) return fail("needs arm=500 and pid=N", target);
    // The process events carry the kernel's process id. If this start runs in the caller's context, that id is the caller's; if it runs
    // in the kernel's own (KERNEL_PID_VALUE), the id the caller passed is the best there is.
    const int own = ksceKernelGetProcessId();
    g_owner = (own > 0 && own != KERNEL_PID_VALUE) ? own : given;
    put("clock: owner process "); put_dec(g_owner); put(" (asked as "); put_dec(given); put(", running as "); put_dec(own); put(")\n");

    // Map the register page (physical 0xE3103000, strongly ordered).
    SceKernelAllocMemBlockKernelOpt opt;
    zero(&opt, sizeof(opt));
    opt.size = sizeof(opt);
    opt.attr = SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_PBASE;
    opt.paddr = BASECLK_PA;
    g_block = ksceKernelAllocMemBlock("infected_clock_io", SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_ROOT_SO_RW, 0x1000, &opt);
    if (g_block < 0) g_block = ksceKernelAllocMemBlock("infected_clock_io", SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_ROOT_DEVICE_RW, 0x1000, &opt);
    if (g_block < 0) return fail("could not map the clock register", g_block);
    void *base = 0;
    if (ksceKernelGetMemBlockBase(g_block, &base) < 0 || !base) return fail("no base for the clock register", 0);
    g_reg = (volatile unsigned int *)base;

    const unsigned int mult = g_reg[0], div = g_reg[1];
    put("clock: register reads multiplier "); put_hex(mult); put(", divisor "); put_hex(div); put("\n");
    if ((mult & ~0xFu) != 0 || (div & 0xFFu) != 0) return fail("register is not at full speed (or not understood): nothing written", (int)mult);

    unsigned int orig, set;
    int write = 1;
    if (mult == 8 || mult == 14) {
        orig = mult; set = mult + 1;                        // 444 MHz step -> the 500 MHz step beside it
    } else if (mult == 9 || mult == 15) {
        if (stale) { orig = mult - 1; set = mult; write = 0; put("clock: 500 MHz left by an earlier run that did not restore it: taking it over\n"); }
        else {
            // Someone else's 500 MHz (a PSVshellPlus profile): nothing to do, nothing to undo.
            put("clock: the register already reads 500 MHz (another plugin or profile): not touching it\n");
            flush_log(1);
            g_owner = -1;
            if (g_block >= 0) { ksceKernelFreeMemBlock(g_block); g_block = -1; }
            g_reg = 0;
            return SCE_KERNEL_START_SUCCESS;
        }
    } else {
        return fail("multiplier is not a 444 or 500 MHz step: nothing written", (int)mult);
    }

    // The safety net first: if the process dies, the old value comes back.
    SceProcEventHandler handler;
    zero(&handler, sizeof(handler));
    handler.size = sizeof(handler);
    handler.exit = on_exit;
    handler.kill = on_kill;
    g_handler = ksceKernelRegisterProcEventHandler("infected_clock", &handler, 0);
    if (g_handler < 0) { const int rc = g_handler; g_handler = -1; return fail("no process handler: not overclocking", rc); }

    g_orig = orig; g_set = set;
    g_engaged = 1;
    if (write) {
        write_mult(set);
        ksceKernelDelayThread(2000);                        // let the PLL settle
    }
    const unsigned int back = g_reg[0];
    put("clock: multiplier "); put_dec((int)orig); put(" -> "); put_dec((int)set); put(", reads "); put_dec((int)back); put("\n");
    if (back != set) {
        restore("write did not stick");
        return fail("the register refused the new value", (int)back);
    }
    flush_log(1);
    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args) {
    (void)argc; (void)args;
    if (g_reg) restore("module stop");
    if (g_handler >= 0) { ksceKernelUnregisterProcEventHandler(g_handler); g_handler = -1; }
    if (g_block >= 0) { ksceKernelFreeMemBlock(g_block); g_block = -1; }
    g_reg = 0;
    return SCE_KERNEL_STOP_SUCCESS;
}
