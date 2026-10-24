// Differential test: lean-generator output (native x86 build of the emitted C++) versus PortableKit's own Allegrex
// interpreter (src/interpreter.cpp) on identical guest memory.  Leaf functions only (the callees of the others are not linked).
//
//   per trial: random arena (mixed pointers / small ints / floats / noise), random args, same initial memory image for both sides;
//   compare return registers the lean signature returns + all of guest RAM except the stack region.
//   Trials where the interpreter faults / exceeds its budget, or where the lean code crashes on a pointer the interpreter
//   tolerated, are counted separately (lean crash on a trial the interpreter completed counts as a FAILURE).
#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/guest_memory.hpp"
#include "psprecomp/interpreter.hpp"
#include "psprecomp/runtime.hpp"
#include "lean.h"

#include <csetjmp>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <vector>
#include <unistd.h>

extern "C" u8 g_ram[32u << 20];
extern "C" void* const g_fn_tbl[1] = {nullptr};
u8 g_ram[32u << 20];

struct Inp { u32 sp; u32 r[32]; float f[32]; };
struct Out { u32 v0 = 0, v1 = 0; float f0 = 0; };
struct Row { u32 addr; int words; int rv0, rv1, rf0; void (*call)(const Inp&, Out&); };

// declarations of the emitted functions are in lean_diff.cpp; the table needs them:
#include "difftest_decls.inc"
static Row g_rows[] = {
#include "difftest_table.inc"
};

static sigjmp_buf g_jmp;
static volatile sig_atomic_t g_in_lean = 0;
static volatile uintptr_t g_fault = 0;
static void on_signal(int sg, siginfo_t* si, void*) { if (g_in_lean) { g_fault = (uintptr_t)si->si_addr; siglongjmp(g_jmp, 1); } std::fprintf(stderr, "SIGNAL %d outside lean at host addr %p\n", sg, si->si_addr); _exit(99); }

static void noop_fn(psprecomp::CorpusRuntime&, psprecomp::AllegrexContext&) {}

int main(int argc, char** argv) {
    const int trials = argc > 1 ? std::atoi(argv[1]) : 8;
    const u32 seed = argc > 2 ? (u32)std::atoi(argv[2]) : 12345u;
    { struct sigaction sa{}; sa.sa_sigaction = on_signal; sa.sa_flags = SA_SIGINFO | SA_NODEFER; sigemptyset(&sa.sa_mask); for (int sg : {SIGSEGV, SIGBUS, SIGALRM, SIGFPE}) sigaction(sg, &sa, nullptr); }
    long crash_outside = 0, crash_inside = 0, c_null = 0, c_scratch = 0, c_vram = 0, c_ram = 0, c_other = 0;

    // image
    std::vector<u8> image;
    { std::ifstream f("/work/infected/work/image.bin", std::ios::binary); image.assign(std::istreambuf_iterator<char>(f), {}); }
    psprecomp::Runtime rt(32u << 20);
    const u32 kSentinel = 0x08FFFFF0u;
    rt.register_function(kSentinel, &noop_fn, "sentinel");
    psprecomp::GuestMemory& mem = rt.memory();
    u8* ip = mem.raw_pointer(0x08000000u, 32u << 20);
    if (!ip) { std::fprintf(stderr, "no raw pointer\n"); return 2; }
    std::memcpy(ip + 0x804000, image.data(), image.size());
    static u8 base[32u << 20];
    std::memcpy(base, ip, sizeof(base));

    const u32 kArena = 0x09000000u, kArenaSize = 1u << 20, kStack = kArena + 0xF0000u, kStackSize = 0x10000u;
    std::mt19937 rng(seed);
    long total = 0, compared = 0, equal = 0, skipped_interp = 0, skipped_budget = 0, lean_crash = 0, mism = 0;
    int funcs_with_compare = 0, funcs_all_equal = 0;
    std::vector<u32> bad;
    const char* only_env = std::getenv("ONLY"); const u32 only = only_env ? (u32)std::strtoul(only_env, nullptr, 0) : 0u;
    for (const Row& row : g_rows) {
        if (only && row.addr != only) continue;
        int cmp_this = 0, eq_this = 0;
        for (int t = 0; t < trials; ++t) {
            ++total;
            std::memcpy(ip, base, sizeof(base));
            std::memcpy(g_ram, base, sizeof(base));
            // build arena
            u8* arena_i = ip + (kArena - 0x08000000u);
            u32* aw = reinterpret_cast<u32*>(arena_i);
            for (u32 i = 0; i < kArenaSize / 4; ++i) {
                u32 k = rng() % 10u, v;
                if (k < 2) v = rng() & 0xFFu;
                else if (k < 6) v = kArena + 0x1000u + ((rng() % 0xE0000u) & ~3u);
                else if (k < 8) { float fl = (float)((int)(rng() % 20001u) - 10000) / 10.0f; std::memcpy(&v, &fl, 4); }
                else v = rng();
                aw[i] = v;
            }
            std::memset(arena_i + (kStack - kArena), 0, kStackSize);
            std::memcpy(g_ram + (kArena - 0x08000000u), arena_i, kArenaSize);
            Inp in{}; in.sp = kStack + kStackSize - 0x100u;
            for (int r = 4; r < 12; ++r) {
                u32 k = rng() % 10u;
                in.r[r] = k < 2 ? (rng() & 0xFFu) : (k < 3 ? (rng() & 0xFFFFFFFFu) : (kArena + 0x10000u + ((rng() % 0x70000u) & ~3u)));
            }
            for (int r = 12; r < 20; ++r) in.f[r] = (float)((int)(rng() % 20001u) - 10000) / 100.0f;
            // interpreter side
            psprecomp::AllegrexContext ctx{};
            for (int r = 4; r < 12; ++r) ctx.gpr[r] = in.r[r];
            for (int r = 12; r < 20; ++r) ctx.fpr[r] = in.f[r];
            ctx.gpr[29] = in.sp; ctx.gpr[31] = kSentinel; ctx.pc = row.addr;
            if (std::getenv("VERBOSE")) { std::fprintf(stderr, "TRIAL %08x %d\n", row.addr, t); }
            psprecomp::InterpreterExit ex;
            try { ex = psprecomp::interpret_allegrex(rt, ctx, 40000u); }
            catch (...) { ++skipped_interp; continue; }
            if (ex != psprecomp::InterpreterExit::Dispatch || ctx.pc != kSentinel) { ++skipped_budget; continue; }
            // lean side (crash-protected)
            Out out{};
            g_in_lean = 1;
            if (sigsetjmp(g_jmp, 1) == 0) { alarm(2); row.call(in, out); alarm(0); }
            else { alarm(0); g_in_lean = 0; ++lean_crash; { u64 ga = (u64)g_fault - ((u64)(uintptr_t)g_ram - 0x08000000u); if (g_fault && (u64)g_fault - (u64)(uintptr_t)g_ram < (32u << 20)) ++crash_inside; else ++crash_outside; if (ga < 0x1000) ++c_null; else if (ga >= 0x00010000 && ga < 0x00014000) ++c_scratch; else if (ga >= 0x04000000 && ga < 0x04200000) ++c_vram; else if (ga >= 0x08000000 && ga < 0x0A000000) ++c_ram; else ++c_other; } bad.push_back(row.addr); if (only) std::printf("trial %d: lean crashed; args r4=%08x r5=%08x r6=%08x r7=%08x sp=%08x\n", t, in.r[4], in.r[5], in.r[6], in.r[7], in.sp); continue; }
            g_in_lean = 0;
            ++compared; ++cmp_this;
            bool ok = true;
            if (row.rv0 && out.v0 != ctx.gpr[2]) ok = false;
            if (row.rv1 && out.v1 != ctx.gpr[3]) ok = false;
            if (row.rf0) { u32 a, b; std::memcpy(&a, &out.f0, 4); b = std::bit_cast<u32>(ctx.fpr[0]); if (a != b && !(out.f0 != out.f0 && ctx.fpr[0] != ctx.fpr[0])) ok = false; }
            // memory: whole RAM except the stack region
            const u32 so = kStack - 0x08000000u;
            if (std::memcmp(ip, g_ram, so) != 0 || std::memcmp(ip + so + kStackSize, g_ram + so + kStackSize, sizeof(base) - so - kStackSize) != 0) ok = false;
            if (!ok && only) {
                std::printf("trial %d MISMATCH: lean v0=%08x v1=%08x f0=%g | interp v0=%08x v1=%08x f0=%g | r4=%08x r5=%08x r6=%08x r7=%08x\n", t, out.v0, out.v1, out.f0, ctx.gpr[2], ctx.gpr[3], ctx.fpr[0], in.r[4], in.r[5], in.r[6], in.r[7]);
                for (u32 o = 0; o < sizeof(base); o += 4) { if (o >= so && o < so + kStackSize) continue; if (std::memcmp(ip + o, g_ram + o, 4)) { std::printf("   mem diff at %08x: interp=%08x lean=%08x\n", 0x08000000u + o, *(u32*)(ip + o), *(u32*)(g_ram + o)); break; } }
            }
            if (ok) { ++equal; ++eq_this; } else { ++mism; bad.push_back(row.addr); }
        }
        if (cmp_this) { ++funcs_with_compare; if (eq_this == cmp_this) ++funcs_all_equal; }
    }
    std::printf("functions=%zu trials/function=%d total_trials=%ld\n", sizeof(g_rows) / sizeof(g_rows[0]), trials, total);
    std::printf("compared=%ld equal=%ld MISMATCH=%ld lean_crash_where_interp_ok=%ld skipped(interp fault)=%ld skipped(budget/other exit)=%ld\n",
                compared, equal, mism, lean_crash, skipped_interp, skipped_budget);
    std::printf("lean crashes: fault address outside guest RAM window=%ld (invalid guest pointer or SIGALRM), inside=%ld\n", crash_outside, crash_inside);
    std::printf("  crash guest-address classes: <0x1000=%ld scratchpad=%ld vram=%ld ram=%ld other(unmapped)=%ld\n", c_null, c_scratch, c_vram, c_ram, c_other);
    std::printf("functions with >=1 compared trial=%d, of which all-equal=%d\n", funcs_with_compare, funcs_all_equal);
    for (size_t i = 0; i < bad.size() && i < 12; ++i) std::printf("  suspect 0x%08X\n", bad[i]);
    return mism || lean_crash ? 1 : 0;
}
