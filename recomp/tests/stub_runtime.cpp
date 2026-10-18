// Tiny stand-in for the real runtime, used by the x86-64 differential tests.
//
//   g_cpu          the live register file
//   g_ram          96 MiB anonymous mapping, so every masked address (a & 0x05FFFFFF) is mapped; guest RAM
//                  0x08000000.. is arena offset 0 (the real arena is 66 MiB, see ARCHITECTURE.md)
//   pk_guest_call  g_lean_entry[...] or a test abort
//   pk_hle_call    deterministic fake: hash of the marshalled arguments (shared with the oracle side of the harness)
//   pk_fatal       aborts the current trial (siglongjmp)
#include "lean_abi.h"
#include "stub_runtime.h"

#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>

psprecomp::AllegrexContext g_cpu;

static uint8_t* map_arena() {
    void* p = mmap(nullptr, 0x06000000u, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) { std::perror("mmap"); std::exit(2); }
    return static_cast<uint8_t*>(p);
}
extern "C" { uint8_t* const g_ram = map_arena(); }

StubState g_stub;

FakeHle fake_hle(uint32_t stub, const uint32_t* a, const float* fa, uint32_t sp, uint32_t counter, int ngpr, int nfpr) {
    uint64_t h = 0x9E3779B97F4A7C15ull ^ stub ^ ((uint64_t)counter << 40);
    auto mix = [&](uint64_t x) { h ^= x + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); h *= 0xFF51AFD7ED558CCDull; h ^= h >> 33; };
    for (int i = 0; i < ngpr && i < 8; ++i) mix(a[i]);
    for (int i = 0; i < nfpr && i < 8; ++i) mix(std::bit_cast<uint32_t>(fa[i]));
    mix(sp);
    FakeHle r;
    const uint32_t k = (uint32_t)(h >> 60);
    r.v0 = k < 3 ? 0u : (k < 6 ? (uint32_t)(h & 0xFF) : (uint32_t)(h >> 8));
    r.v1 = (uint32_t)(h >> 20);
    r.f0 = (float)((int32_t)(h >> 11) % 100000) / 100.0f;
    return r;
}

uint32_t g_hle_stack_lo = 0, g_hle_stack_hi = 0, g_hle_dead_hi = 0;       // snapshot mode: RAM offsets of the recorded thread's stack (out-pointers there get one word)

void fake_hle_writes(uint8_t* ram, uint32_t stub, const uint32_t* a, int ngpr, uint32_t counter) {
    for (int i = 0; i < ngpr && i < 8; ++i) {
        const uint32_t p = a[i];
        const uint32_t po = p & 0x05FFFFFFu;
        const bool in_snap_stack = g_hle_stack_hi && p >= 0x08000000u && p < 0x0A000000u && po >= g_hle_stack_lo && po + 4 <= g_hle_stack_hi;
        if (!in_snap_stack && (p < 0x09000000u || p >= 0x0904C000u - 16u)) continue;
        if (p & 3u) continue;
        uint64_t h = 0xA5A5F00D12345678ull ^ stub ^ ((uint64_t)counter << 32) ^ ((uint64_t)i << 56);
        // heap buffer: 12 bytes; stack variable: one word (never reaches a neighbouring save slot); in snapshot mode sceCtrlPeekBufferPositive's
        // buffer gets 16 bytes when it lies wholly inside the dead frame (below the entry sp), words 1..3 zero (SceCtrlData: timestamp, buttons = 0)
        const int nw = in_snap_stack ? ((stub == 0x08A89A2Cu && i == 0 && po + 16 <= g_hle_dead_hi) ? 4 : 1) : (p < 0x09010000u ? 3 : 1);
        for (int k = 0; k < nw; ++k) {
            h = h * 6364136223846793005ull + 1442695040888963407ull;
            const uint32_t w = (in_snap_stack && k > 0) ? 0u : (uint32_t)(h >> 32);
            std::memcpy(ram + (p & 0x05FFFFFFu) + 4 * k, &w, 4);
        }
    }
}

extern "C" void pk_hle_call(uint32_t stub_pc) {
    ++g_stub.hle_calls;
    const int ngpr = stub_arity_gpr(stub_pc), nfpr = stub_arity_fpr(stub_pc);
    uint32_t a[8]; float fa[8];
    for (int i = 0; i < 8; ++i) { a[i] = g_cpu.gpr[4 + i]; fa[i] = g_cpu.fpr[12 + i]; }
    fake_hle_writes(g_ram, stub_pc, a, ngpr, g_stub.hle_counter);
    if (getenv("LEAN_HLE_TRACE")) std::fprintf(stderr, "  lean HLE %08x n=%d a=%08x %08x %08x %08x sp=%08x ctr=%u\n", stub_pc, ngpr, a[0], a[1], a[2], a[3], g_cpu.gpr[29], g_stub.hle_counter);
    const FakeHle r = fake_hle(stub_pc, a, fa, g_cpu.gpr[29], g_stub.hle_counter++, ngpr, nfpr);
    g_cpu.gpr[2] = r.v0; g_cpu.gpr[3] = r.v1; g_cpu.fpr[0] = r.f0;
}

extern "C" void pk_guest_call(uint32_t pc) {
    ++g_stub.guest_calls;
    const uint32_t idx = (pc - LEAN_BASE) >> 2;
    if ((pc & 3u) || pc < LEAN_BASE || idx >= g_lean_entry_count || !g_lean_entry[idx]) {
        g_stub.abort_reason = "indirect call to a non-function";
        g_stub.abort_pc = pc;
        siglongjmp(g_stub.jmp, 1);
    }
    if (++g_stub.depth > 4000) { g_stub.abort_reason = "call depth"; siglongjmp(g_stub.jmp, 1); }
    g_lean_entry[idx]();
    --g_stub.depth;
}

extern "C" [[noreturn]] void pk_fatal(const char* what, uint32_t pc) {
    g_stub.abort_reason = what;
    g_stub.abort_pc = pc;
    g_stub.fatal = true;
    siglongjmp(g_stub.jmp, 1);
}

extern "C" void pk_yield(void) {}
