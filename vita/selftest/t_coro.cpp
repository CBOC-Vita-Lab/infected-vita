// T3 - coroutines: hand-written ARMv7 context switch (selftest/coro_arm.S), 4 coroutines with 256 KiB stacks.
//   star  : scheduler <-> coroutine, N round trips (the shape of a guest-kernel "run thread i, come back")
//   ring  : coroutine -> coroutine direct (the shape of a guest scheduler switching guest threads)
//   c++   : C++ bodies entered through the trampoline, recursing ~200 KiB deep, switching at the bottom, verifying frames
// Every resume verifies r4-r11 and d8-d15 (see coro_arm.S) and the 64-byte stack canaries. Cost is reported in ns per switch.
#include "st_common.h"

namespace st {

void t3_coroutines() {
#if defined(__arm__)
    const uint32_t kStack = 256 * 1024;
    const uint32_t hops = (uint32_t)cfg_int("t3.pingpongs", 100000);
    plat::MemBlock blk;
    if (!plat::mem_alloc_block("coro-stacks", 4u * kStack, 4096, &blk)) {
        fail("T3", "alloc", "cannot allocate 4 x %u KiB of coroutine stacks", kStack / 1024);
        return;
    }
    CoroResult r = {};
    struct Mode { uint32_t mode; const char* name; uint32_t n; };
    const Mode modes[] = {{0, "star", hops}, {1, "ring", hops * 2}, {2, "c++deep", hops / 20 > 50 ? hops / 20 : 50}};
    double ns_star = 0, ns_ring = 0;
    for (const Mode& m : modes) {
        status("T3 coroutines: %s %u hops", m.name, m.n);
        // warm-up (touches the stacks' first pages, fills caches)
        st_coro_run(m.mode, 64, kStack, blk.base, &r);
        uint64_t t0 = plat::now_us();
        uint32_t errs = st_coro_run(m.mode, m.n, kStack, blk.base, &r);
        uint64_t dt = plat::now_us() - t0;
        double ns = dt * 1000.0 / (r.switches ? r.switches : 1);
        if (m.mode == 0) ns_star = ns;
        if (m.mode == 1) ns_ring = ns;
        bool ok = errs == 0 && r.last_seen_mask == 0xF && r.switches >= m.n;
        check(ok, "T3", m.name, "%u switches in %llu us = %.1f ns/switch (%.0f cycles at 444 MHz), register/frame/canary errors %u, coroutines run mask 0x%X%s",
              r.switches, (unsigned long long)dt, ns, ns * 0.444, errs, r.last_seen_mask,
              m.mode == 2 ? " (includes the recursion work: not a pure switch cost)" : "");
        if (m.mode == 2) info("T3", "c++deep", "recursion depth reached by coroutine 0: %u levels (about %u KiB of its %u KiB stack)", r.deep_levels_max, r.deep_levels_max * 520 / 1024, kStack / 1024);
        poll_abort("T3");
    }
    info("T3", "summary", "switch cost: star %.1f ns, ring %.1f ns; 500k-switch budget per second would use %.1f%% of one core",
         ns_star, ns_ring, ns_ring * 500000.0 / 1e9 * 100.0);
    plat::mem_free_block(&blk);
#else
    skip("T3", "coroutines", "the hand-written switch is ARMv7 only (validated off-target with scripts/check-coro.sh in Unicorn)");
#endif
}

}  // namespace st
