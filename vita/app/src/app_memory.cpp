// app_memory.cpp - process memory budget (Vita, ATTRIBUTE2 = extended memory mode; see CMakeLists VITA_ATTRIBUTE2).
//
// Budget (MiB), what each consumer takes from the user pool (USER_RW, cached unless noted):
//   newlib heap        NEWLIB_HEAP_MIB, one block reserved at start-up through _newlib_heap_size_user. It holds the C++ runtime's
//                      own data (GE state, texture cache copies, decoded movie frames, sas buffers, std::map / vector state, FFmpeg
//                      contexts) and the 256 KiB host stacks of the guest threads (coroutines). The default of the SDK is 128 MiB.
//   guest arena        66 MiB (0x04200000): 32 MiB guest RAM + scratchpad + 2 MiB VRAM + padding, one sceKernelAllocMemBlock block.
//   vitaGL RAM pool    gl_ram_pool_mib (24): USER_RW_UNCACHE, vertex/index streams and textures that do not fit CDRAM.
//   vitaGL CDRAM pool  all of CDRAM (128 MiB on retail, ~120 free): render targets, textures, GXM buffers.
//   code + data        ~7 MB of generated game code, ~3.7 MB lean table, runtime, FFmpeg decoders, vitaGL, ~1.5 MB of .data/.bss.
//   thread stacks      main thread sceUserMainThreadStackSize, plat::Thread stacks (256 KiB default), audio pump, vitaGL GC.
// The log prints the measured numbers (memory_line) at start-up, after the GL init, after the load and once a minute.
#include "app_memory.h"

#include <malloc.h>
#include <stdio.h>

#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>

#include <vitaGL.h>

#include "platform.h"

#ifndef NEWLIB_HEAP_MIB
#define NEWLIB_HEAP_MIB 128
#endif

// ---- symbols the SDK start-up code reads (C linkage; const objects need an explicit extern in C++ to have external linkage)
extern "C" {
unsigned int _newlib_heap_size_user = (unsigned int)NEWLIB_HEAP_MIB * 1024u * 1024u;
extern const char sceUserMainThreadName[];
extern const int sceUserMainThreadPriority;
extern const unsigned int sceUserMainThreadStackSize;
extern const unsigned int sceUserMainThreadCpuAffinityMask;
const char sceUserMainThreadName[] = "infected-main";
const int sceUserMainThreadPriority = 0x10000100;                          // SCE_KERNEL_DEFAULT_PRIORITY_USER
const unsigned int sceUserMainThreadStackSize = 1024u * 1024u;             // scheduler + HLE + the renderer when it runs on this thread
const unsigned int sceUserMainThreadCpuAffinityMask = 0x10000u;            // SCE_KERNEL_CPU_MASK_USER_0: the guest runs on core 0
}

namespace app {
namespace {
plat::MemBlock g_arena;
size_t g_peak = 0;
}  // namespace

uint8_t* arena_allocate(size_t bytes) {
    if (g_arena.base) return (uint8_t*)g_arena.base;
    if (!plat::mem_alloc_block("guest-arena", bytes, 1024 * 1024, &g_arena)) {
        plat::logf("memory: cannot allocate the %u MiB guest arena", (unsigned)(bytes >> 20));
        return nullptr;
    }
    plat::logf("memory: guest arena %u MiB at %p (uid %d)", (unsigned)(g_arena.size >> 20), g_arena.base, g_arena.uid);
    return (uint8_t*)g_arena.base;
}

void arena_release(uint8_t* arena, size_t) {
    if (arena && arena == g_arena.base) plat::mem_free_block(&g_arena);
}

size_t arena_bytes() { return g_arena.size; }

void memory_sample() {
    struct mallinfo mi = mallinfo();
    if ((size_t)mi.arena > g_peak) g_peak = (size_t)mi.arena;
}

size_t heap_peak() { return g_peak; }

std::string memory_line() {
    memory_sample();
    plat::MemInfo m;
    plat::mem_info(&m);
    char buf[400];
    snprintf(buf, sizeof(buf),
             "mem: user_free=%d KiB cdram_free=%d KiB phycont_free=%d KiB | heap taken=%u KiB used=%u KiB peak=%u KiB of %u KiB | vgl ram %u/%u KiB "
             "cdram %u/%u KiB",
             (int)(m.user_free / 1024), (int)(m.cdram_free / 1024), (int)(m.phycont_free / 1024), (unsigned)(m.malloc_arena / 1024),
             (unsigned)(m.malloc_used / 1024), (unsigned)(g_peak / 1024), (unsigned)(_newlib_heap_size_user / 1024),
             plat::gl_is_ready() ? (unsigned)(vglMemFree(VGL_MEM_RAM) / 1024) : 0u, plat::gl_is_ready() ? (unsigned)(vglMemTotal(VGL_MEM_RAM) / 1024) : 0u,
             plat::gl_is_ready() ? (unsigned)(vglMemFree(VGL_MEM_VRAM) / 1024) : 0u, plat::gl_is_ready() ? (unsigned)(vglMemTotal(VGL_MEM_VRAM) / 1024) : 0u);
    return buf;
}

}  // namespace app
