// selftest/st_console_vita.cpp - CPU framebuffer on the display (no GXM, no vitaGL) used for the progress panel before GL
// starts and when GL is unavailable. Same approach as vitasdk's debugScreen: a 2 MiB CDRAM block (256 KiB aligned size),
// ABGR8888, pitch 960 pixels, sceDisplaySetFrameBuf(NEXTFRAME).
#include "st_common.h"

#include <psp2/display.h>
#include <psp2/kernel/sysmem.h>

namespace st {

namespace {
SceUID g_blk = -1;
uint32_t* g_fb = nullptr;
}  // namespace

bool console_open() {
    if (g_fb) return true;
    g_blk = sceKernelAllocMemBlock("st-console", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 2 * 1024 * 1024, nullptr);
    if (g_blk < 0) {
        plat::logf("console: CDRAM block failed 0x%08X (continuing without an on-screen panel)", (unsigned)g_blk);
        g_blk = -1;
        return false;
    }
    void* base = nullptr;
    sceKernelGetMemBlockBase(g_blk, &base);
    g_fb = (uint32_t*)base;
    console_refresh();
    SceDisplayFrameBuf fb = {};
    fb.size = sizeof(fb);
    fb.base = g_fb;
    fb.pitch = 960;
    fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    fb.width = 960;
    fb.height = 544;
    int rc = sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
    plat::logf("console: framebuffer %p, sceDisplaySetFrameBuf -> 0x%08X", (void*)g_fb, (unsigned)rc);
    return true;
}

void console_close() {
    if (!g_fb) return;
    sceDisplaySetFrameBuf(nullptr, SCE_DISPLAY_SETBUF_IMMEDIATE);
    sceKernelFreeMemBlock(g_blk);
    g_blk = -1;
    g_fb = nullptr;
}

void console_refresh() {
    if (g_fb) panel_draw(g_fb, 960, 960, 544);
}

}  // namespace st
