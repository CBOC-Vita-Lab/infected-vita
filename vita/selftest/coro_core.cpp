// selftest/coro_core.cpp - freestanding coroutine scheduler used by T3. No libc, no platform dependency, so the very same
// object code can be run instruction-by-instruction in an emulator (scripts/validate_coro_unicorn.py).
#include <stddef.h>
#include <stdint.h>

#include "st_common.h"

extern "C" {
void st_co_switch(uint32_t* from, const uint32_t* to);
void st_co_tramp(void);
void st_co_body(void);   // actually takes B* in r0 (entered through st_co_tramp)
}

namespace {
constexpr int kCo = 4;                  // coroutines
constexpr uint32_t kCtxWords = 26;
constexpr uint32_t kCanary = 0xC0DEC0DEu;
constexpr uint32_t kCanaryWords = 16;   // 64 bytes at the lowest addresses of every stack

struct B {                              // layout is known to coro_arm.S - keep in sync
    uint32_t* self_ctx;
    uint32_t* next_ctx;
    uint32_t* sched_ctx;
    uint32_t id;
    uint32_t seed;
    uint32_t errors;
    uint32_t runs;
    uint32_t* counter;
    // C-entry bookkeeping (mode 2)
    uint32_t deep_levels;
    uint32_t deep_errors;
};
static_assert(offsetof(B, counter) == 28, "asm offset");

alignas(8) uint32_t g_ctx[kCo + 1][kCtxWords];
B g_b[kCo];
uint32_t g_counter;

// ---- mode 2: C++ coroutine bodies that recurse deep, switch away at the bottom of the recursion and verify every frame
// after resuming. Exercises the stack of each coroutine (256 KiB) and the compiler-generated prologue/epilogue code.
struct Frame { volatile uint32_t w[110]; };   // 440 bytes of payload per level (+ saved regs)

__attribute__((noinline)) uint32_t deep(B* b, uint32_t pat, uint32_t levels) {
    Frame f;
    for (uint32_t i = 0; i < 110; i++) f.w[i] = pat * 2654435761u + i + levels;
    uint32_t errs = 0;
    if (levels) {
        errs += deep(b, pat + 1, levels - 1);
    } else {
        // bottom of the recursion: hand control to the next coroutine / scheduler
        if (--*b->counter == 0) st_co_switch(b->self_ctx, b->sched_ctx);
        else st_co_switch(b->self_ctx, b->next_ctx);
    }
    for (uint32_t i = 0; i < 110; i++)
        if (f.w[i] != pat * 2654435761u + i + levels) errs++;
    return errs;
}

__attribute__((noreturn)) void st_co_c_entry(B* b) {
    for (;;) {
        b->seed++;
        uint32_t levels = b->deep_levels - ((b->seed & 3) * 8);   // vary depth a bit
        b->deep_errors += deep(b, b->id * 1000 + b->seed, levels);
        b->runs++;
    }
}
}  // namespace

extern "C" uint32_t st_coro_run(uint32_t mode, uint32_t hops, uint32_t stack_bytes, void* stack_mem, st::CoroResult* out) {
    uint8_t* mem = (uint8_t*)stack_mem;
    for (uint32_t i = 0; i < sizeof(g_ctx) / 4; i++) ((uint32_t*)g_ctx)[i] = 0;
    for (int i = 0; i < kCo; i++) {
        uint32_t* canary = (uint32_t*)(mem + (size_t)i * stack_bytes);
        for (uint32_t k = 0; k < kCanaryWords; k++) canary[k] = kCanary;
        B& b = g_b[i];
        b.self_ctx = g_ctx[i];
        b.sched_ctx = g_ctx[kCo];
        b.next_ctx = (mode == 1 || mode == 2) ? g_ctx[(i + 1) % kCo] : g_ctx[kCo];
        b.id = (uint32_t)i;
        b.seed = 0;
        b.errors = 0;
        b.runs = 0;
        b.counter = &g_counter;
        b.deep_errors = 0;
        // each frame ~ 440 + 40 bytes; keep 16 KiB headroom
        b.deep_levels = (stack_bytes > 32768) ? (stack_bytes - 16384) / 520 - 40 : 8;
        uint32_t* c = g_ctx[i];
        c[0] = (uint32_t)(uintptr_t)&b;                                          // r4 = argument
        c[1] = (mode == 2) ? (uint32_t)(uintptr_t)&st_co_c_entry : (uint32_t)(uintptr_t)&st_co_body;   // r5 = entry
        c[8] = (uint32_t)(uintptr_t)((mem + (size_t)(i + 1) * stack_bytes) ) & ~7u;                     // sp
        c[9] = (uint32_t)(uintptr_t)&st_co_tramp;                                // lr
    }
    uint32_t switches = 0;
    if (mode == 0) {
        // star: scheduler <-> coroutine i, `hops` round trips
        g_counter = 0xFFFFFFFFu;
        for (uint32_t r = 0; r < hops; r++) {
            st_co_switch(g_ctx[kCo], g_ctx[r & (kCo - 1)]);
            switches += 2;
        }
    } else {
        // ring: 0 -> 1 -> 2 -> 3 -> 0 ..., `hops` switches, control returns to the scheduler when the counter hits zero
        g_counter = hops;
        st_co_switch(g_ctx[kCo], g_ctx[0]);
        switches = hops + 1;
    }
    uint32_t errors = 0, mask = 0;
    for (int i = 0; i < kCo; i++) {
        errors += g_b[i].errors + g_b[i].deep_errors;
        if (g_b[i].runs) mask |= 1u << i;
        uint32_t* canary = (uint32_t*)(mem + (size_t)i * stack_bytes);
        for (uint32_t k = 0; k < kCanaryWords; k++)
            if (canary[k] != kCanary) errors++;
    }
    out->switches = switches;
    out->errors = errors;
    out->last_seen_mask = mask;
    out->deep_levels_max = g_b[0].deep_levels;
    return errors;
}
