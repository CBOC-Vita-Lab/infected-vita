// selftest/t_sys.cpp - T1 clocks, T2 memory, T4 threads, T10 file IO.
#include "st_common.h"

#include <stdio.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

namespace st {

plat::MemBlock g_arena;

namespace {
constexpr size_t MiB = 1024 * 1024;

double mib_s(uint64_t bytes, uint64_t us) { return us ? (double)bytes / (double)MiB / ((double)us / 1e6) : 0.0; }

void log_mem(const char* test, const char* tag) {
    plat::MemInfo m;
    if (!plat::mem_info(&m)) return;
    info(test, tag, "free: user %lld KiB, cdram %lld KiB, phycont %lld KiB | newlib heap: arena %lld KiB used %lld KiB free %lld KiB",
         (long long)(m.user_free / 1024), (long long)(m.cdram_free / 1024), (long long)(m.phycont_free / 1024),
         (long long)(m.malloc_arena / 1024), (long long)(m.malloc_used / 1024), (long long)(m.malloc_free / 1024));
}

volatile uint32_t g_sink;
}  // namespace

// ============================================================================================================ T1
void t1_clocks() {
    plat::ClockReport before = plat::clocks_read();
    info("T1", "clocks-as-found", "arm %d bus %d gpu %d xbar %d MHz", before.arm, before.bus, before.gpu, before.xbar);
    plat::ClockReport r = plat::clocks_set_max();
    info("T1", "clocks-set", "requested arm %d bus %d gpu %d xbar %d | read back arm %d bus %d gpu %d xbar %d | setter rc %d/%d/%d/%d",
         r.req_arm, r.req_bus, r.req_gpu, r.req_xbar, r.arm, r.bus, r.gpu, r.xbar, r.rc_arm, r.rc_bus, r.rc_gpu, r.rc_xbar);
    if (plat::kind() == plat::Kind::Vita) {
        bool ok = r.arm == 444 && r.bus == 222 && r.gpu == 222 && r.xbar == 166;
        check(ok, "T1", "max-clocks", "ARM %d (want 444), BUS %d (222), GPU %d (222), XBAR %d (166) MHz", r.arm, r.bus, r.gpu, r.xbar);
    } else {
        skip("T1", "max-clocks", "no clock control on %s", plat::name());
    }

    // ---- timer sanity: monotonic, resolution, cost of a read
    const int N = 200000;
    uint64_t prev = plat::now_us(), t0 = prev, back = 0, steps = 0, maxstep = 0;
    for (int i = 0; i < N; i++) {
        uint64_t t = plat::now_us();
        if (t < prev) back++;
        if (t != prev) { steps++; if (t - prev > maxstep) maxstep = t - prev; }
        prev = t;
    }
    uint64_t dt = prev - t0;
    check(back == 0, "T1", "timer-monotonic", "%d reads, %llu went backwards, %llu distinct steps (resolution %.2f us), %.0f ns per now_us()",
          N, (unsigned long long)back, (unsigned long long)steps, steps ? (double)dt / (double)steps : 0.0, dt * 1000.0 / N);

    // ---- sleep granularity (T4 relies on it for pacing)
    const uint32_t reqs[] = {1000, 2000, 10000, 16667};
    for (uint32_t req : reqs) {
        uint64_t mn = ~0ull, mx = 0, sum = 0;
        const int K = 12;
        for (int i = 0; i < K; i++) {
            uint64_t a = plat::now_us();
            plat::sleep_us(req);
            uint64_t d = plat::now_us() - a;
            if (d < mn) mn = d;
            if (d > mx) mx = d;
            sum += d;
        }
        bool ok = mn + 50 >= req && sum / K < req * 3 + 3000;
        check(ok, "T1", "sleep", "sleep_us(%u): min %llu avg %llu max %llu us over %d tries", req, (unsigned long long)mn, (unsigned long long)(sum / K),
              (unsigned long long)mx, K);
    }
    {
        uint64_t a = plat::now_us();
        for (int i = 0; i < 2000; i++) plat::yield();
        info("T1", "yield", "plat::yield(): %.2f us per call", (double)(plat::now_us() - a) / 2000.0);
    }

    // ---- CPU baselines (single thread, 150 ms each): used to judge T3/T4/T8 numbers
    {
        uint32_t x = 1;
        uint64_t iters = 0, a = plat::now_us(), end = a + 150000;
        while (plat::now_us() < end) {
            for (int i = 0; i < 4096; i++) x = x * 1664525u + 1013904223u;   // dependent chain: 1 mul-add per iteration
            iters += 4096;
        }
        g_sink = x;
        uint64_t d = plat::now_us() - a;
        info("T1", "cpu-int", "dependent mul-add chain: %.1f M iterations/s (%.2f ns each)", (double)iters / (double)d, (double)d * 1000.0 / (double)iters);
        float f = 1.0f, g = 0.5f;
        iters = 0;
        a = plat::now_us();
        end = a + 150000;
        while (plat::now_us() < end) {
            for (int i = 0; i < 4096; i++) { f = f * 1.0000001f + 0.0000001f; g = g * 0.9999999f + 0.0000002f; }
            iters += 4096;
        }
        g_sink = (uint32_t)(f + g);
        d = plat::now_us() - a;
        info("T1", "cpu-float", "2 independent float mul-add chains: %.1f M iterations/s (%.2f ns each)", (double)iters / (double)d, (double)d * 1000.0 / (double)iters);
    }
}

// ============================================================================================================ T2
void t2_memory() {
    log_mem("T2", "before");
    const size_t kArena = 66 * MiB;

    // ---- the guest arena (kept for the rest of the run)
    if (!g_arena.base) {
        uint64_t a = plat::now_us();
        bool ok = plat::mem_alloc_block("guest-arena", kArena, 1 * MiB, &g_arena);
        uint64_t d = plat::now_us() - a;
        if (!ok) {
            fail("T2", "arena-alloc", "66 MiB arena allocation failed (see log). Consider ATTRIBUTE2 extended memory or a smaller _newlib_heap_size_user");
            log_mem("T2", "after-failed-arena");
            return;
        }
        pass("T2", "arena-alloc", "66 MiB at %p (raw %p, raw size %zu KiB, uid %d) in %llu us", g_arena.base, g_arena.raw_base, g_arena.raw_size / 1024, g_arena.uid,
             (unsigned long long)d);
    }
    uint64_t touch = plat::mem_touch_pages(g_arena.base, g_arena.size);
    info("T2", "arena-touch", "touching %zu pages took %llu us (%.1f MiB/s of page faults)", g_arena.size / 4096, (unsigned long long)touch,
         mib_s(g_arena.size, touch));
    log_mem("T2", "after-arena");

    // ---- guest address mapping used by the architecture: host(a) = base + (a & 0x05FFFFFF)
    {
        uint8_t* base = (uint8_t*)g_arena.base;
        struct { uint32_t guest; const char* what; } probes[] = {
            {0x08000000u, "user RAM start"}, {0x0BFFFFFFu, "user RAM last byte"}, {0x04000000u, "VRAM start"},
            {0x041FFFFFu, "VRAM last byte"}, {0x48000000u, "user RAM, cached-mirror bit set"}, {0x0A123456u, "user RAM interior"},
        };
        bool ok = true;
        for (auto& p : probes) {
            size_t off = p.guest & 0x05FFFFFFu;
            if (off >= g_arena.size) { ok = false; info("T2", "arena-map", "guest %08X (%s) -> offset %zu is OUTSIDE the arena", p.guest, p.what, off); continue; }
            base[off] = (uint8_t)(p.guest >> 3);
        }
        for (auto& p : probes) {
            size_t off = p.guest & 0x05FFFFFFu;
            if (off < g_arena.size && base[off] != (uint8_t)(p.guest >> 3)) ok = false;
        }
        // the 48000000 mirror is expected to alias 08000000 after masking
        check(ok, "T2", "arena-map", "host(a)=base+(a&0x05FFFFFF) stays inside the 66 MiB arena for user RAM and VRAM probes");
    }

    // ---- bandwidth on the arena (cached USER_RW memory)
    {
        uint8_t* base = (uint8_t*)g_arena.base;
        uint64_t a = plat::now_us();
        memset(base, 0xA5, g_arena.size);
        uint64_t dw = plat::now_us() - a;
        a = plat::now_us();
        uint32_t sum = 0;
        const uint32_t* p = (const uint32_t*)base;
        for (size_t i = 0; i < g_arena.size / 4; i += 8) sum += p[i] + p[i + 1] + p[i + 2] + p[i + 3] + p[i + 4] + p[i + 5] + p[i + 6] + p[i + 7];
        uint64_t dr = plat::now_us() - a;
        g_sink = sum;
        a = plat::now_us();
        memcpy(base, base + 32 * MiB, 32 * MiB);
        uint64_t dc = plat::now_us() - a;
        memset(base, 0, g_arena.size);   // leave it zeroed
        info("T2", "arena-bandwidth", "memset %.0f MiB/s, 32-bit read sum %.0f MiB/s, memcpy 32 MiB %.0f MiB/s", mib_s(g_arena.size, dw), mib_s(g_arena.size, dr), mib_s(32 * MiB, dc));
    }

    // ---- newlib heap: 96 MiB
    {
        const size_t kBig = 96 * MiB;
        uint64_t a = plat::now_us();
        uint8_t* p = (uint8_t*)malloc(kBig);
        uint64_t d = plat::now_us() - a;
        if (!p) {
            // find the largest block that works, in 8 MiB steps
            size_t best = 0;
            for (size_t s = kBig - 8 * MiB; s >= 8 * MiB; s -= 8 * MiB) {
                void* q = malloc(s);
                if (q) { best = s; free(q); break; }
            }
            fail("T2", "newlib-96MiB", "malloc(96 MiB) returned NULL; largest block found: %zu MiB (the newlib heap is _newlib_heap_size_user = 144 MiB in this build; the arena above is NOT part of it)", best / MiB);
            log_mem("T2", "after-failed-malloc");
        } else {
            uint64_t tt = plat::mem_touch_pages(p, kBig);
            // pattern write + verify: one word per 4 KiB page plus the last word
            for (size_t off = 0; off < kBig; off += 4096) *(uint32_t*)(p + off) = (uint32_t)(off >> 12) * 2654435761u;
            size_t bad = 0;
            for (size_t off = 0; off < kBig; off += 4096)
                if (*(uint32_t*)(p + off) != (uint32_t)(off >> 12) * 2654435761u) bad++;
            check(bad == 0, "T2", "newlib-96MiB", "malloc(96 MiB) = %p in %llu us, touch %llu us, %zu pages verified, %zu bad", (void*)p, (unsigned long long)d,
                  (unsigned long long)tt, kBig / 4096, bad);
            log_mem("T2", "after-malloc-96MiB");
            // what is left in the newlib heap while the big block is live?
            void* q = malloc(32 * MiB);
            info("T2", "newlib-extra", "second malloc(32 MiB) while 96 MiB are live: %s", q ? "succeeded" : "NULL (heap exhausted - expected when the heap is < 128 MiB + 32 MiB)");
            free(q);
            free(p);
            log_mem("T2", "after-free-96MiB");
        }
    }

    // ---- headroom probe: how many more 16 MiB blocks does the user pool give us? (shows extended-memory mode working)
    {
        plat::MemBlock blocks[24];
        int n = 0;
        for (; n < 24; n++) {
            if (!plat::mem_alloc_block("probe", 16 * MiB, 4096, &blocks[n])) break;
        }
        info("T2", "headroom", "additional 16 MiB USER_RW blocks obtainable on top of arena + newlib heap: %d (%d MiB)", n, n * 16);
        log_mem("T2", "at-peak");
        for (int i = 0; i < n; i++) plat::mem_free_block(&blocks[i]);
        log_mem("T2", "after-probe-free");
    }
}

// ============================================================================================================ T4
namespace {
struct BusyArg {
    int want_core;
    int core_start = -1, core_end = -1;
    uint64_t iters = 0;
    uint64_t ran_us = 0;
    volatile bool go = false;
    volatile bool done = false;
    uint32_t duration_us = 1000000;
};

void busy_fn(void* a) {
    BusyArg* b = (BusyArg*)a;
    b->core_start = plat::thread_current_core();
    while (!b->go) plat::yield();
    uint32_t x = 1;
    uint64_t t0 = plat::now_us(), end = t0 + b->duration_us, n = 0;
    while (plat::now_us() < end) {
        for (int i = 0; i < 4096; i++) x = x * 1664525u + 1013904223u;
        n += 4096;
    }
    g_sink = x;
    b->ran_us = plat::now_us() - t0;
    b->iters = n;
    b->core_end = plat::thread_current_core();
    b->done = true;
}

struct PingArg {
    plat::Event* to_worker;
    plat::Event* to_main;
    int rounds;
    volatile bool stop = false;
    int core = -1;
};
void ping_fn(void* a) {
    PingArg* p = (PingArg*)a;
    p->core = plat::thread_current_core();
    for (;;) {
        plat::event_wait(p->to_worker, -1);
        if (p->stop) break;
        plat::event_signal(p->to_main);
    }
    plat::event_signal(p->to_main);
}

void latency_test(const char* label, int worker_core, int rounds) {
    PingArg pa;
    pa.to_worker = plat::event_create(true);
    pa.to_main = plat::event_create(true);
    pa.rounds = rounds;
    plat::ThreadDesc d;
    d.name = "t4-ping";
    d.fn = ping_fn;
    d.arg = &pa;
    d.core = worker_core;
    d.stack_bytes = 64 * 1024;
    plat::Thread* th = plat::thread_start(d);
    if (!th) {
        fail("T4", label, "could not start the ping thread on core %d", worker_core);
        plat::event_destroy(pa.to_worker);
        plat::event_destroy(pa.to_main);
        return;
    }
    // warm-up
    for (int i = 0; i < 50; i++) { plat::event_signal(pa.to_worker); plat::event_wait(pa.to_main, 1000000); }
    static uint32_t samples[4096];
    int n = rounds > 4096 ? 4096 : rounds;
    uint64_t sum = 0, mx = 0;
    int timeouts = 0;
    for (int i = 0; i < n; i++) {
        uint64_t a = plat::now_us();
        plat::event_signal(pa.to_worker);
        if (!plat::event_wait(pa.to_main, 1000000)) timeouts++;
        uint64_t rtt = plat::now_us() - a;
        samples[i] = (uint32_t)rtt;
        sum += rtt;
        if (rtt > mx) mx = rtt;
    }
    pa.stop = true;
    plat::event_signal(pa.to_worker);
    plat::event_wait(pa.to_main, 1000000);
    plat::thread_join(th);
    // median / p99 of the round-trip time
    for (int i = 1; i < n; i++) { uint32_t v = samples[i]; int j = i - 1; while (j >= 0 && samples[j] > v) { samples[j + 1] = samples[j]; j--; } samples[j + 1] = v; }
    check(timeouts == 0, "T4", label, "event round trip (signal worker, worker signals back) over %d rounds: avg %.1f us, median %u us, p99 %u us, max %llu us -> one-way ~%.1f us; worker ran on core %d; %d timeouts",
          n, (double)sum / n, samples[n / 2], samples[(n * 99) / 100], (unsigned long long)mx, (double)sum / n / 2.0, pa.core, timeouts);
    plat::event_destroy(pa.to_worker);
    plat::event_destroy(pa.to_main);
}
}  // namespace

void t4_threads() {
    const uint32_t dur_ms = (uint32_t)cfg_int("t4.ms", 1000);
    info("T4", "main-thread", "running on core %d", plat::thread_current_core());

    // ---- baseline: one busy thread alone on core 0 for a quarter of the time
    double base_rate = 0;
    {
        BusyArg b;
        b.want_core = 0;
        b.duration_us = dur_ms * 250;
        plat::ThreadDesc d;
        d.name = "t4-base";
        d.fn = busy_fn;
        d.arg = &b;
        d.core = 0;
        d.stack_bytes = 64 * 1024;
        plat::Thread* th = plat::thread_start(d);
        if (!th) { fail("T4", "baseline", "could not start a thread"); return; }
        b.go = true;
        plat::thread_join(th);
        base_rate = (double)b.iters / (double)b.ran_us;   // M iterations / s
        info("T4", "baseline", "one thread alone on core 0: %.1f M iterations/s (core at start %d, at end %d)", base_rate, b.core_start, b.core_end);
    }

    // ---- three pinned threads at once, one per user core
    BusyArg args[3];
    plat::Thread* th[3] = {};
    for (int i = 0; i < 3; i++) {
        args[i].want_core = i;
        args[i].duration_us = dur_ms * 1000;
        plat::ThreadDesc d;
        d.name = i == 0 ? "t4-core0" : (i == 1 ? "t4-core1" : "t4-core2");
        d.fn = busy_fn;
        d.arg = &args[i];
        d.core = i;
        d.stack_bytes = 64 * 1024;
        th[i] = plat::thread_start(d);
        if (!th[i]) fail("T4", "start", "thread for core %d could not be created", i);
    }
    for (int i = 0; i < 3; i++) args[i].go = true;
    uint64_t t0 = plat::now_us();
    for (int i = 0; i < 3; i++) if (th[i]) plat::thread_join(th[i]);
    uint64_t wall = plat::now_us() - t0;
    bool all_done = true;
    double total = 0;
    for (int i = 0; i < 3; i++) {
        if (!th[i] || !args[i].done) { all_done = false; continue; }
        double rate = (double)args[i].iters / (double)args[i].ran_us;
        total += rate;
        bool core_ok = args[i].core_start == i && args[i].core_end == i;
        double rel = base_rate > 0 ? rate / base_rate : 0;
        if (core_ok) info("T4", "core", "thread pinned to core %d: ran on %d..%d, %.1f M it/s = %.0f%% of the single-thread baseline", i, args[i].core_start, args[i].core_end, rate, rel * 100.0);
        else limit("T4", "core", "thread pinned to core %d reported core %d at start and %d at end (%.1f M it/s = %.0f%% of baseline) - UNVERIFIED which index sceKernelGetCpuId reports; Vita3K may not model cores", i, args[i].core_start, args[i].core_end, rate, rel * 100.0);
    }
    check(all_done, "T4", "three-threads", "3 pinned threads ran %u ms each (wall %llu ms); aggregate %.1f M it/s = %.2fx the single-thread baseline (3.0x = perfect scaling)",
          dur_ms, (unsigned long long)(wall / 1000), total, base_rate > 0 ? total / base_rate : 0.0);
    poll_abort("T4");

    // ---- event latency: same core and cross-core
    const int rounds = cfg_int("t4.rounds", 2000);
    status("T4 event latency");
    latency_test("event-same-core", 0, rounds);
    latency_test("event-cross-core-1", 1, rounds);
    latency_test("event-cross-core-2", 2, rounds);

    // ---- timeout accuracy of event_wait
    {
        plat::Event* ev = plat::event_create(true);
        uint64_t mn = ~0ull, mx = 0, sum = 0;
        const int K = 10;
        for (int i = 0; i < K; i++) {
            uint64_t a = plat::now_us();
            bool got = plat::event_wait(ev, 5000);
            uint64_t d = plat::now_us() - a;
            if (got) { fail("T4", "event-timeout", "unsignalled event reported signalled"); break; }
            if (d < mn) mn = d;
            if (d > mx) mx = d;
            sum += d;
        }
        check(mn + 100 >= 5000, "T4", "event-timeout", "event_wait(5000 us) on an unsignalled event: min %llu avg %llu max %llu us", (unsigned long long)mn, (unsigned long long)(sum / K), (unsigned long long)mx);
        plat::event_destroy(ev);
    }
    // ---- priority API round trip
    {
        bool a = plat::thread_set_priority(nullptr, -4);
        bool b = plat::thread_set_priority(nullptr, 0);
        if (a && b) pass("T4", "priority", "thread_set_priority(self, -4) and back to 0 accepted");
        else limit("T4", "priority", "thread_set_priority(self, -4)=%d, (0)=%d - UNVERIFIED which priority range real hardware accepts", (int)a, (int)b);
    }
}

// ============================================================================================================ T10
void t10_file_io() {
    const size_t kTotal = (size_t)cfg_int("t10.mib", 16) * MiB;
    const char* name = "selftest_io.bin";

    // ---- app0: (read-only package) visibility
    {
        void* data = nullptr;
        size_t size = 0;
        if (plat::file_read_all(plat::Root::App, "sce_sys/param.sfo", &data, &size)) {
            pass("T10", "app-root", "read app0:sce_sys/param.sfo (%zu bytes, magic %02X %02X %02X %02X)", size, ((uint8_t*)data)[0], ((uint8_t*)data)[1], ((uint8_t*)data)[2], ((uint8_t*)data)[3]);
            free(data);
        } else {
            limit("T10", "app-root", "app0:sce_sys/param.sfo not readable on this platform (%s); expected on Vita and in Vita3K", plat::name());
        }
    }

    // ---- write 16 MiB (1 MiB chunks), verify later by CRC
    uint8_t* buf = (uint8_t*)memalign(64, 1 * MiB);
    if (!buf) { fail("T10", "alloc", "no 1 MiB buffer"); return; }
    uint32_t crc_w = 0;
    {
        plat::File* f = plat::file_open(plat::Root::Data, name, plat::FileMode::WriteTruncate, 0);
        if (!f) { fail("T10", "create", "cannot create %s in the data root", name); free(buf); return; }
        uint64_t a = plat::now_us();
        for (size_t off = 0; off < kTotal; off += 1 * MiB) {
            for (size_t i = 0; i < 1 * MiB / 4; i++) ((uint32_t*)buf)[i] = (uint32_t)((off / 4 + i) * 2654435761u);
            crc_w = crc32_update(crc_w, buf, 1 * MiB);
            if (plat::file_write(f, buf, 1 * MiB) != 1 * MiB) { fail("T10", "write", "short write at %zu", off); break; }
        }
        plat::file_close(f);
        uint64_t d = plat::now_us() - a;
        info("T10", "write", "wrote %zu MiB in 1 MiB chunks: %.1f MiB/s (includes generating and CRC-ing the data)", kTotal / MiB, mib_s(kTotal, d));
    }
    poll_abort("T10");

    // ---- sequential reads with different chunk sizes
    struct Case { size_t chunk; size_t buffer; const char* label; };
    const Case cases[] = {
        {1 * MiB, 0, "1 MiB reads"}, {256 * 1024, 0, "256 KiB reads"}, {64 * 1024, 0, "64 KiB reads"},
        {16 * 1024, 0, "16 KiB reads"}, {4 * 1024, 0, "4 KiB reads (unbuffered)"}, {4 * 1024, 256 * 1024, "4 KiB reads through a 256 KiB read-ahead buffer"},
    };
    double best = 0;
    for (const Case& c : cases) {
        plat::File* f = plat::file_open(plat::Root::Data, name, plat::FileMode::Read, c.buffer);
        if (!f) { fail("T10", "open", "cannot reopen %s", name); break; }
        uint32_t crc = 0;
        size_t total = 0;
        uint64_t spent_crc = 0;
        uint64_t a = plat::now_us();
        for (;;) {
            size_t n = plat::file_read(f, buf, c.chunk);
            if (!n) break;
            uint64_t c0 = plat::now_us();
            crc = crc32_update(crc, buf, n);
            spent_crc += plat::now_us() - c0;
            total += n;
        }
        uint64_t d = plat::now_us() - a - spent_crc;   // time excluding the CRC work
        plat::file_close(f);
        double r = mib_s(total, d);
        if (r > best) best = r;
        bool ok = total == kTotal && crc == crc_w;
        check(ok, "T10", "read", "%s: %.1f MiB/s (%zu bytes, crc %08X%s)", c.label, r, total, crc, crc == crc_w ? "" : " MISMATCH");
        poll_abort("T10");
    }
    info("T10", "summary", "best sequential read %.1f MiB/s; advice: >= 64 KiB reads, 64-byte aligned buffer; note ux0: reads right after a write may be served from the OS cache and look faster than a cold read", best);
    plat::file_remove(plat::Root::Data, name);
    free(buf);
}

}  // namespace st
