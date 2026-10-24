// infected_core3.skprx: lets the application put threads on the PS Vita's fourth CPU core (the CPU 3 affinity mask), which the
// kernel otherwise reserves for the system. The application loads it from its own package at start-up (taiLoadStartKernelModule,
// taiHEN is there on every HENkaku/Ensō console) and unloads it when it quits, so the player installs nothing.
//
// It hooks one function of SceKernelThreadMgr, the affinity check thread creation and sceKernelChangeThreadCpuAffinityMask call,
// so that no mask is refused. Nothing else is changed. The function's place (segment 0, offset 0x114C, Thumb) is the one
// CapUnlocker by GrapheneCt (MIT, https://github.com/GrapheneCt/CapUnlocker) hooks on firmware 3.60 to 3.74. This module checks
// the console's real firmware (not a spoofed one) and refuses to start on any other, hooking nothing.
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/sysroot.h>
#include <taihen.h>

static tai_hook_ref_t g_ref;
static SceUID g_hook = -1;

static int affinity_check_patched(int a1, int a2, int a3) {
    (void)a1;
    (void)a2;
    (void)a3;
    return 0;   // "not an illegal affinity"
}

int _start(SceSize argc, const void *args) __attribute__((weak, alias("module_start")));
int module_start(SceSize argc, const void *args) {
    (void)argc;
    (void)args;
    const int firmware = ksceKernelSysrootGetSystemSwVersion();
    if (firmware < 0x03600000 || firmware >= 0x03750000) return SCE_KERNEL_START_FAILED;
    tai_module_info_t info;
    info.size = sizeof(info);
    if (taiGetModuleInfoForKernel(KERNEL_PID, "SceKernelThreadMgr", &info) < 0) return SCE_KERNEL_START_FAILED;
    g_hook = taiHookFunctionOffsetForKernel(KERNEL_PID, &g_ref, info.modid, 0, 0x114C, 1, affinity_check_patched);
    return g_hook >= 0 ? SCE_KERNEL_START_SUCCESS : SCE_KERNEL_START_FAILED;
}

int module_stop(SceSize argc, const void *args) {
    (void)argc;
    (void)args;
    if (g_hook >= 0) taiHookReleaseForKernel(g_hook, g_ref);
    g_hook = -1;
    return SCE_KERNEL_STOP_SUCCESS;
}
