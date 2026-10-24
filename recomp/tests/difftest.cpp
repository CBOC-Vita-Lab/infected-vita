// Function-level differential test: lean-generated code (native x86-64) versus PortableKit's Allegrex interpreter.
//
// For every guest function (typed and regfile form) and N random trials:
//   * identical register file, VFPU state, and guest RAM (the real image + a random pointer-plausible arena + stack)
//     on both sides;
//   * oracle: interpret_allegrex() from the function entry until it returns to a sentinel; import stubs are answered by
//     the shared fake HLE (hash of the arguments); callees run in the interpreter;
//   * lean: the function's entry thunk through g_lean_entry[] (g_cpu convention); callees run lean; jalr goes through
//     pk_guest_call -> g_lean_entry;
//   * compare the contract's returned registers, all of guest RAM (page-tracked, except the callee's dead frame below
//     the entry sp), the VFPU registers / control, fcr31 (without the condition bit).
// Trials in which the oracle cannot finish (unmapped access, budget, unsupported op, jalr to non-code) are skipped.
// A trial where the oracle finishes and lean aborts, crashes or times out is a MISMATCH.
#include "lean_abi.h"
#include "stub_runtime.h"

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/guest_memory.hpp"
#include "psprecomp/interpreter.hpp"
#include "psprecomp/runtime.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>
#include <dirent.h>

struct TestRow { uint32_t addr; int regfile; uint32_t gpr_mask, fpr_mask; int at; int size; uint32_t sgpr_mask, sfpr_mask, pgpr_mask, pfpr_mask; };
struct StubRow { uint32_t stub; int ngpr, nfpr, hgpr, hfpr; };
#include "lean_test_meta.inc"

int stub_arity_gpr(uint32_t stub) { for (auto& s : kStubs) if (s.stub == stub) return s.hgpr; return 0; }   // hashed args: defined at every call site
int stub_arity_fpr(uint32_t stub) { for (auto& s : kStubs) if (s.stub == stub) return s.hfpr; return 0; }

using psprecomp::AllegrexContext;
static constexpr uint32_t kRamBase = 0x08000000u, kRamSize = 32u << 20;
static constexpr uint32_t kSentinel = 0x08FFFFF0u;
static constexpr uint32_t kArena = 0x09000000u, kArenaSize = 0x10000u, kStack = kArena + kArenaSize, kStackSize = 0x10000u;
static constexpr uint32_t kWinSize = kArenaSize + kStackSize;
static constexpr uint32_t kStackTop = kStack + kStackSize;
static constexpr uint32_t kSp0 = kStack + 0xC000u;      // entry sp: [sp0, top) is the caller's area

// ------------------------------------------------------------------------------------------ page tracking
// Both RAM copies are write-protected page by page (host pages; the interpreter's vector is not page aligned, so each
// side has its own phase).  A write faults once, marks the page dirty and unprotects it.  Everything is compared in RAM
// offset space: the union of both sides' dirty pages, plus the "always" ranges (first/last 8 KiB, the arena/stack
// window with a 4 KiB margin), which are refreshed from the pristine image at the start of every trial.
static constexpr uint32_t kPage = 4096;
struct Side {
    uint8_t* lo = nullptr; uint8_t* hi = nullptr; uint8_t* ap = nullptr;
    size_t npages = 0, phase = 0;
    std::vector<uint8_t> tracked, dirty_bit;
    std::vector<uint32_t> dirty;                       // page indices
};
static Side g_side[2];                                 // 0 = interpreter, 1 = lean
static volatile sig_atomic_t g_in_lean = 0;
static uint8_t* g_base = nullptr;                      // pristine RAM image (32 MiB)
static const uint32_t kWinLo = kArena - kRamBase, kWinHi = kWinLo + kWinSize;
static const uint32_t kAlways[3][2] = {{0u, 8192u}, {kRamSize - 8192u, kRamSize}, {kWinLo - kPage, kWinHi + kPage}};

static constexpr uint32_t kScratchBase = 0x00010000u;
static constexpr uint32_t kVramOff = 0x04000000u, kVramBytes = 0x200000u, kScratchOff = 0x10000u, kScratchBytes = 0x4000u;
static uint8_t* g_ivram = nullptr;                      // the interpreter's VRAM + scratchpad buffer
static volatile sig_atomic_t g_vram_dirty = 0;

static void on_segv(int sg, siginfo_t* si, void*) {
    const uintptr_t a = (uintptr_t)si->si_addr;
    for (int i = 0; i < 2; ++i) {
        Side& sd = g_side[i];
        if (a >= (uintptr_t)sd.ap && a < (uintptr_t)sd.ap + sd.npages * kPage) {
            const size_t k = (a - (uintptr_t)sd.ap) / kPage;
            if (sd.tracked[k]) {
                if (!sd.dirty_bit[k]) { sd.dirty_bit[k] = 1; sd.dirty.push_back((uint32_t)k); }
                mprotect(sd.ap + k * kPage, kPage, PROT_READ | PROT_WRITE);
                return;
            }
        }
    }
    if (a >= (uintptr_t)g_ram + kVramOff && a < (uintptr_t)g_ram + kVramOff + kVramBytes && g_vram_dirty == 0) {
        g_vram_dirty = 1;
        mprotect(g_ram + kVramOff, kVramBytes, PROT_READ | PROT_WRITE);
        return;
    }
    if (g_in_lean && ((a >= (uintptr_t)g_ram + 0x02000000u && a < (uintptr_t)g_ram + kVramOff) ||
                      (a >= (uintptr_t)g_ram + kVramOff + kVramBytes && a < (uintptr_t)g_ram + 0x06000000u))) {
        g_stub.abort_reason = "unmodelled arena region";            // VRAM mirrors / the arena hole: not part of the contract
        g_stub.abort_pc = (uint32_t)(a - (uintptr_t)g_ram);
        siglongjmp(g_stub.jmp, 1);
    }
    if (g_in_lean) {
        g_stub.abort_reason = sg == SIGALRM ? "timeout" : "host fault";
        g_stub.abort_pc = (uint32_t)a;
        siglongjmp(g_stub.jmp, 1);
    }
    std::fprintf(stderr, "SIGNAL %d outside lean at %p\n", sg, si->si_addr);
    _exit(99);
}
static void on_alarm(int) {
    if (g_in_lean) { g_stub.abort_reason = "timeout"; siglongjmp(g_stub.jmp, 1); }
}

static void setup_side(Side& sd, uint8_t* lo) {
    sd.lo = lo; sd.hi = lo + kRamSize;
    sd.ap = (uint8_t*)(((uintptr_t)lo + kPage - 1) & ~(uintptr_t)(kPage - 1));
    sd.phase = (size_t)(sd.ap - lo);
    sd.npages = (size_t)(sd.hi - sd.ap) / kPage;
    sd.tracked.assign(sd.npages, 0); sd.dirty_bit.assign(sd.npages, 0);
    for (size_t k = 0; k < sd.npages; ++k) {
        const uint32_t s = (uint32_t)(sd.phase + k * kPage), e = s + kPage;
        bool inside = false;
        for (auto& r : kAlways) if (s >= r[0] && e <= r[1]) inside = true;
        sd.tracked[k] = !inside;
    }
    // protect runs of tracked pages
    size_t k = 0;
    while (k < sd.npages) {
        if (!sd.tracked[k]) { ++k; continue; }
        size_t j = k; while (j < sd.npages && sd.tracked[j]) ++j;
        mprotect(sd.ap + k * kPage, (j - k) * kPage, PROT_READ);
        k = j;
    }
}

static void restore_dirty() {
    for (Side& sd : g_side) {
        for (uint32_t k : sd.dirty) {
            const size_t s = sd.phase + (size_t)k * kPage;
            std::memcpy(sd.lo + s, g_base + s, kPage);
            mprotect(sd.ap + (size_t)k * kPage, kPage, PROT_READ);
            sd.dirty_bit[k] = 0;
        }
        sd.dirty.clear();
    }
}

// VRAM (0x04000000, 2 MiB) and the scratchpad (0x00010000, 16 KiB) are separate host buffers in the interpreter and
// part of the arena in lean code (ARCHITECTURE.md section 1): compare them across.

static void protect_vram() { mprotect(g_ram + kVramOff, kVramBytes, PROT_READ); g_vram_dirty = 0; }

// returns the differences found (also resets both sides to zero when anything was touched)
static void special_compare_reset(std::vector<std::string>* out) {
    const bool vdiff = std::memcmp(g_ivram, g_ram + kVramOff, kVramBytes) != 0;
    if (vdiff && out) out->push_back("vram differs");
    if (out && std::memcmp(g_ivram + kVramBytes, g_ram + kScratchOff, kScratchBytes) != 0) out->push_back("scratchpad differs");
    if (vdiff || g_vram_dirty) {
        std::memset(g_ivram, 0, kVramBytes);
        mprotect(g_ram + kVramOff, kVramBytes, PROT_READ | PROT_WRITE);
        std::memset(g_ram + kVramOff, 0, kVramBytes);
        protect_vram();
    mprotect(g_ram + 0x02000000u, 0x02000000u, PROT_NONE);
    mprotect(g_ram + kVramOff + kVramBytes, 0x06000000u - kVramOff - kVramBytes, PROT_NONE);
    }
    std::memset(g_ivram + kVramBytes, 0, kScratchBytes);
}

// Install the trial's initial memory: pristine image everywhere, the trial window over the arena/stack.
static void install_memory(const std::vector<uint8_t>& win) {
    static std::vector<uint8_t> tmp;
    for (Side& sd : g_side) {
        std::memcpy(sd.lo + kAlways[0][0], g_base + kAlways[0][0], kAlways[0][1] - kAlways[0][0]);
        std::memcpy(sd.lo + kAlways[1][0], g_base + kAlways[1][0], kAlways[1][1] - kAlways[1][0]);
    }
    tmp.assign(g_base + kAlways[2][0], g_base + kAlways[2][1]);
    std::memcpy(tmp.data() + (kWinLo - kAlways[2][0]), win.data(), kWinSize);
    for (Side& sd : g_side) std::memcpy(sd.lo + kAlways[2][0], tmp.data(), tmp.size());
}

// ------------------------------------------------------------------------------------------ state snapshots
struct Cpu {
    uint32_t gpr[32]; uint32_t hi, lo; float fpr[32]; uint32_t fcr31; float vfpu[128]; uint32_t ctrl[16];
};
static void cpu_to_ctx(const Cpu& c, AllegrexContext& x) {
    for (int i = 0; i < 32; ++i) { x.gpr[i] = c.gpr[i]; x.fpr[i] = c.fpr[i]; }
    x.hi = c.hi; x.lo = c.lo; x.fcr31 = c.fcr31;
    for (int i = 0; i < 128; ++i) x.vfpu[i] = c.vfpu[i];
    for (int i = 0; i < 16; ++i) x.vfpu_ctrl[i] = c.ctrl[i];
}
static void ctx_to_cpu(const AllegrexContext& x, Cpu& c) {
    for (int i = 0; i < 32; ++i) { c.gpr[i] = x.gpr[i]; c.fpr[i] = x.fpr[i]; }
    c.hi = x.hi; c.lo = x.lo; c.fcr31 = x.fcr31;
    for (int i = 0; i < 128; ++i) c.vfpu[i] = x.vfpu[i];
    for (int i = 0; i < 16; ++i) c.ctrl[i] = x.vfpu_ctrl[i];
}
static bool same_bits(float a, float b) { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); }

// ------------------------------------------------------------------------------------------ generators
static std::vector<uint32_t> g_entries;               // function entry addresses
static std::vector<uint32_t> g_code_ptrs;             // some pointers into the image (data / code)
static uint32_t g_image_end = 0;

static FILE* g_stats_file = nullptr;   // --stats FILE: per function: address, compared trials, mismatching trials
static bool g_special = false;     // --special: NaNs, infinities, denormals, signed zeros and rounding boundaries in float inputs
static bool g_strict_nan = false;  // --strict-nan: NaN-vs-NaN differences are mismatches too
// (no signalling NaNs: a compiler may fold `-0.0f - x` into `-x`, which keeps an sNaN signalling where the interpreter's
//  subtraction quiets it; neither the guest nor the game depends on that)
static const uint32_t kSpecialBits[] = {
    0x00000000u, 0x80000000u, 0x3F800000u, 0xBF800000u, 0x3F000000u, 0xBF000000u, 0x40000000u, 0x40400000u, 0x3E800000u, 0x3EAAAAABu,
    0x00000001u, 0x80000001u, 0x0000FFFFu, 0x007FFFFFu, 0x807FFFFFu, 0x00800000u, 0x80800000u, 0x7F7FFFFFu, 0xFF7FFFFFu, 0x7F800000u, 0xFF800000u,
    0x7FC00000u, 0xFFC00000u, 0x7FC00001u, 0xFFC00001u, 0x7FC12345u, 0x7FFFFFFFu, 0xFFFFFFFFu, 0x4F000000u, 0x4EFFFFFFu, 0xCF000000u, 0xCEFFFFFFu,
    0x3F800001u, 0x3F7FFFFFu, 0x3EFFFFFFu, 0x3F000001u, 0x3FC00000u, 0xBFC00000u, 0x40200000u, 0xC0200000u, 0x4B000000u, 0x4B000001u, 0x4B800000u,
    0x42FE0000u, 0x437F0000u, 0x43800000u, 0x477FFF00u, 0x47800000u, 0x38D1B717u, 0x3C23D70Au, 0x41200000u, 0xC1200000u, 0x42C80000u };
static uint32_t special_bits(std::mt19937_64& rng) { return kSpecialBits[rng() % (sizeof kSpecialBits / sizeof kSpecialBits[0])]; }

struct Gen {
    std::mt19937_64 rng;
    int mode = 1;                                   // 0 pointer heavy, 1 mixed, 2 small integers
    uint32_t r(uint32_t n) { return (uint32_t)(rng() % n); }
    uint32_t arena_ptr(uint32_t align = 4) {
        uint32_t o = 0x800u + (r(kArenaSize - 0x1000u) & ~(align - 1u));
        return kArena + o;
    }
    uint32_t stack_ptr() { return (kSp0 + 0x10u + (r(kStackTop - kSp0 - 0x40u) & ~3u)); }
    // VRAM mirrors (guest 0x04200000..0x047FFFFF) are not modelled by the lean memory macro (ARCHITECTURE.md section 1)
    static bool mirror(uint32_t v) { const uint32_t c = v & 0x1FFFFFFFu; return c >= 0x04200000u && c < 0x04800000u; }
    uint32_t word(bool in_arena) { uint32_t v; do v = word0(in_arena); while (mirror(v)); return v; }
    uint32_t arg() { uint32_t v; do v = arg0(); while (mirror(v)); return v; }
    uint32_t word0(bool in_arena) {
        if (mode == 0) {
            const uint32_t q = r(100);
            if (q < 62) return arena_ptr(r(2) ? 16 : 4);
            if (q < 78) return r(64);
            if (q < 84) return 0;
            if (q < 90) return std::bit_cast<uint32_t>((float)((int32_t)r(2001) - 1000) / 16.0f);
            if (q < 94) return g_entries[r((uint32_t)g_entries.size())];
            return r(4096);
        }
        const uint32_t k = r(100);
        if (k < 18) return r(64);
        if (k < 24) return 0;
        if (k < 30) return r(0x10000);
        if (k < 34) return (uint32_t)-(int32_t)r(64);
        if (k < 58) return arena_ptr();
        if (k < 60) return kScratchBase + (r(kScratchBytes) & ~3u);
        if (k < 62) return 0x04000000u + (r(kVramBytes) & ~3u);
        if (k < 70) { float f = (float)((int32_t)r(20001) - 10000) / (r(2) ? 10.0f : 1000.0f); return std::bit_cast<uint32_t>(f); }
        if (k < 74) return g_entries[r((uint32_t)g_entries.size())];
        if (k < 78) return 0x08804000u + (r(g_image_end - 0x08804000u) & ~3u);
        if (k < 82) return stack_ptr();
        return (uint32_t)rng();
    }
    uint32_t arg0() {
        if (mode == 0 && r(100) < 60) return arena_ptr(r(3) == 0 ? 16 : 4);
        if (mode == 2 && r(100) < 70) return r(r(2) ? 16 : 300);
        const uint32_t k = r(100);
        if (k < 36) return arena_ptr(r(3) == 0 ? 16 : 4);
        if (k < 37) return kScratchBase + (r(kScratchBytes) & ~3u);
        if (k < 38) return 0x04000000u + (r(kVramBytes) & ~3u);
        if (k < 52) return r(64);
        if (k < 56) return 0;
        if (k < 64) return r(0x10000);
        if (k < 68) return (uint32_t)-(int32_t)r(16);
        if (k < 74) return stack_ptr();
        if (k < 80) return 0x08804000u + (r(g_image_end - 0x08804000u) & ~3u);
        if (k < 84) return g_entries[r((uint32_t)g_entries.size())];
        if (k < 90) return 1u << r(32);
        return (uint32_t)rng();
    }
    float farg() {
        const uint32_t k = r(100);
        if (k < 10) return 0.0f;
        if (k < 20) return 1.0f;
        if (k < 30) return -1.0f;
        if (k < 80) return (float)((int32_t)r(20001) - 10000) / 100.0f;
        if (k < 90) return (float)((int32_t)r(2001) - 1000) / 8.0f;
        return std::bit_cast<float>((uint32_t)rng() & 0x7F7FFFFFu);
    }
};

struct SnapPage { uint32_t off; std::vector<uint8_t> data; };
struct Trial {
    Cpu cpu;                                   // initial register state
    std::vector<uint8_t> win;                  // arena + stack contents
    std::vector<SnapPage> pages;               // snapshot mode: the guest RAM pages the recorded call touched
};

// ------------------------------------------------------------------------------------------ snapshot mode (--snap DIR)
// Real game state: files written by the patched interpreter (tests/snapshot/portablekit-snapshot.patch) at the entry of a
// function: the register file and the RAM pages the call touched.  Both sides start from the pristine image plus those pages.
static bool g_snap_mode = false;
static std::string g_snap_dir;
static std::map<uint32_t, std::vector<std::string>> g_snap_files;     // entry -> files
static uint32_t g_dead_lo = 0, g_dead_hi = 0;                        // RAM offsets: the callee's dead frame below the entry sp (not compared)
static int g_lean_alarm = 2;
static std::vector<std::pair<uint32_t, uint32_t>> g_hash_ranges;       // snapshot mode: [lo, hi) RAM offsets hashed by the tracer
struct SnapHeader {
    char magic[8];
    uint32_t entry, occurrence, ra, flags;
    uint32_t gpr[32], hi, lo, fcr31, fpr[32], vfpu[128], ctrl[16];
    uint32_t npages, page_bytes;
    uint64_t instructions;
};
static uint32_t g_snap_flags = 0;
static bool load_snapshot(Trial& t, const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    SnapHeader h;
    bool ok = std::fread(&h, sizeof h, 1, f) == 1 && std::memcmp(h.magic, "LSNP1", 5) == 0;
    t.pages.clear();
    for (uint32_t i = 0; ok && i < h.npages; ++i) {
        SnapPage p; p.data.resize(h.page_bytes);
        ok = std::fread(&p.off, 4, 1, f) == 1 && std::fread(p.data.data(), 1, h.page_bytes, f) == h.page_bytes;
        if (ok) t.pages.push_back(std::move(p));
    }
    std::fclose(f);
    if (!ok) return false;
    std::memset(&t.cpu, 0, sizeof t.cpu);
    for (int i = 0; i < 32; ++i) { t.cpu.gpr[i] = h.gpr[i]; t.cpu.fpr[i] = std::bit_cast<float>(h.fpr[i]); }
    t.cpu.hi = h.hi; t.cpu.lo = h.lo; t.cpu.fcr31 = h.fcr31;
    for (int i = 0; i < 128; ++i) t.cpu.vfpu[i] = std::bit_cast<float>(h.vfpu[i]);
    for (int i = 0; i < 16; ++i) t.cpu.ctrl[i] = h.ctrl[i];
    t.cpu.gpr[31] = kSentinel;
    t.win.assign(kWinSize, 0);
    g_snap_flags = h.flags;
    const uint32_t sp = h.gpr[29] & 0x05FFFFFFu;
    g_dead_hi = sp; g_dead_lo = sp > 0x20000u ? sp - 0x20000u : 0u;
    g_hle_stack_lo = g_dead_lo; g_hle_stack_hi = 0x02000000u; g_hle_dead_hi = g_dead_hi;
    return true;
}
static void install_trial(const Trial& t) {
    install_memory(t.win);
    g_hash_ranges.clear();
    for (const SnapPage& p : t.pages) {
        uint32_t lo = p.off, hi = p.off + (uint32_t)p.data.size();
        if (lo < g_dead_hi && hi > g_dead_lo) {            // cut the dead frame out
            if (lo < g_dead_lo) g_hash_ranges.push_back({lo, g_dead_lo});
            if (hi > g_dead_hi) g_hash_ranges.push_back({g_dead_hi, hi});
        } else g_hash_ranges.push_back({lo, hi});
    }
    for (const SnapPage& p : t.pages)
        for (Side& sd : g_side) std::memcpy(sd.lo + p.off, p.data.data(), p.data.size());
}

static void make_trial(Trial& t, Gen& g, bool junk) {
    std::memset(&t.cpu, 0, sizeof t.cpu);
    for (int i = 4; i < 12; ++i) t.cpu.gpr[i] = g.arg();
    for (int i = 12; i < 20; ++i) t.cpu.fpr[i] = (g_special && g.r(100) < 40) ? std::bit_cast<float>(special_bits(g.rng)) : g.farg();
    t.cpu.gpr[28] = kGp; t.cpu.gpr[29] = kSp0; t.cpu.gpr[31] = kSentinel;
    t.cpu.ctrl[0] = 0xE4; t.cpu.ctrl[1] = 0xE4; t.cpu.ctrl[2] = 0;
    t.cpu.fcr31 = g.r(4);
    for (int i = 0; i < 128; ++i)
        t.cpu.vfpu[i] = (g_special && g.r(100) < 35) ? std::bit_cast<float>(special_bits(g.rng)) : (float)((int32_t)g.r(2001) - 1000) / 16.0f;
    t.cpu.ctrl[3] = g.r(256);
    if (junk) {
        for (int i : {1, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 30}) {
            if (i >= 4 && i < 12) continue;
            t.cpu.gpr[i] = g.word(false);
        }
        for (int i = 1; i < 12; ++i) t.cpu.fpr[i] = g.farg();
        for (int i = 20; i < 32; ++i) t.cpu.fpr[i] = g.farg();
        t.cpu.hi = (uint32_t)g.rng(); t.cpu.lo = (uint32_t)g.rng();
    }
    t.win.assign(kWinSize, 0);
    uint32_t* w = reinterpret_cast<uint32_t*>(t.win.data());
    for (uint32_t i = 0; i < kArenaSize / 4; ++i) w[i] = (g_special && g.r(100) < 30) ? special_bits(g.rng) : g.word(true);
    // stack: the caller's area above sp0 holds plausible words, below sp0 is the callee's dead frame
    const uint32_t so = (kStack - kArena) / 4;
    for (uint32_t i = 0; i < kStackSize / 4; ++i) w[so + i] = (kStack + i * 4 >= kSp0) ? g.word(true) : 0u;
}

// ------------------------------------------------------------------------------------------ hook cases (--hooks)
// The generator's native replacements of the game's memcpy, memset and strcmp (lean_abi.h) are compared with the interpreter on
// targeted arguments instead of random ones: every length class, overlap in both directions at every small distance (the guest
// memcpy is a forward byte loop, so a destination inside its own source repeats a pattern), uncached aliases, strings that agree
// up to a chosen byte, signed bytes.  Run with --hooks --list <file with the three addresses> (and again with LEAN_NO_HOOKS=all to
// check the plain translation too).
static bool g_hooks = false;
static void hook_case(Trial& t, Gen& g, uint32_t entry, int tr) {
    uint8_t* arena = t.win.data();
    const auto at = [&](uint32_t a) { return arena + (a - kArena); };
    const uint32_t lo = kArena + 0x1000u, span = 0x6000u;        // keep every range inside the arena
    static const uint32_t kLens[] = {0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 255, 256, 257, 1000, 4096, 5000};
    const uint32_t n = (tr % 5 == 0) ? kLens[g.r(sizeof kLens / sizeof kLens[0])] : (tr % 5 == 1 ? g.r(40) : g.r(3) == 0 ? g.r(600) : kLens[g.r(14)]);
    if (entry == 0x08A2F228u) {                                   // memcpy(dst, src, n): forward byte loop, returns dst
        static const int32_t kDeltas[] = {0, 1, -1, 2, -2, 3, -3, 4, -4, 5, 7, -7, 8, -8, 15, 16, -16, 17, 31, 33, -33};
        uint32_t base = lo + g.r(span), dst, src;
        int32_t delta;
        switch (tr % 4) {
            case 0: delta = kDeltas[g.r(sizeof kDeltas / sizeof kDeltas[0])]; break;
            case 1: delta = (int32_t)n; if (g.r(2)) delta = -delta; delta += (int32_t)g.r(3) - 1; break;    // abutting / one overlapping byte
            case 2: delta = (int32_t)g.r(2 * n + 1) - (int32_t)n; break;                                       // anywhere around the source
            default: delta = (int32_t)g.r(0x2000) - 0x1000; break;                                            // disjoint, either order
        }
        src = base; dst = (uint32_t)((int32_t)base + delta);
        if (dst < lo) dst = lo + g.r(64);
        if (dst + n >= lo + span + 0x3000u) dst = lo + g.r(64);
        if (src + n >= lo + span + 0x3000u) src = lo;
        t.cpu.gpr[4] = dst; t.cpu.gpr[5] = src; t.cpu.gpr[6] = n;
        if (g.r(6) == 0) t.cpu.gpr[5] |= 0x40000000u;             // uncached alias of the source
        else if (g.r(6) == 0) t.cpu.gpr[4] |= 0x40000000u;
        else if (g.r(10) == 0) { t.cpu.gpr[4] ^= 0x08000000u; t.cpu.gpr[5] ^= 0x08000000u; }   // 0x00.. / 0x48.. spelling of the same RAM
        for (uint32_t i = 0; i < 0x9000u && lo - kArena + i < kArenaSize; ++i) at(lo)[i] = (uint8_t)g.rng();
    } else if (entry == 0x08A2F2FCu) {                            // memset(dst, c, n): low byte of c, returns dst
        const uint32_t dst = lo + g.r(span);
        t.cpu.gpr[4] = dst; t.cpu.gpr[5] = g.r(3) == 0 ? (uint32_t)g.rng() : g.r(256); t.cpu.gpr[6] = n;
        if (g.r(8) == 0) t.cpu.gpr[4] |= 0x40000000u;
        for (uint32_t i = 0; i < 0x9000u && lo - kArena + i < kArenaSize; ++i) at(lo)[i] = (uint8_t)g.rng();
    } else if (entry == 0x08A2F47Cu) {                            // strcmp(a, b): signed bytes, stops at a differing byte or a NUL in a
        const uint32_t len = g.r(3) == 0 ? g.r(300) : g.r(24);
        const uint32_t a = lo + g.r(0x2000), b = lo + 0x3000u + g.r(0x2000);
        const bool high = g.r(3) == 0;                            // bytes >= 0x80 (negative as signed chars)
        for (uint32_t i = 0; i <= len + 8; ++i) {
            uint8_t c = (uint8_t)(1 + g.r(high ? 254 : 126));
            if (i < len) at(a)[i] = at(b)[i] = c;
            else at(a)[i] = at(b)[i] = (uint8_t)g.rng();
        }
        at(a)[len] = 0; at(b)[len] = g.r(4) == 0 ? (uint8_t)(1 + g.r(250)) : 0;     // b may continue past a's end
        if (len != 0u && g.r(3) != 0) { const uint32_t k = g.r(len); at(b)[k] = (uint8_t)(g.r(4) == 0 ? 0 : 1 + g.r(254)); }   // differ at k (maybe equal by chance)
        if (g.r(5) == 0) at(a)[0] = 0;
        t.cpu.gpr[4] = a; t.cpu.gpr[5] = b;
        if (g.r(6) == 0) t.cpu.gpr[5] |= 0x40000000u;
    }
}

// ------------------------------------------------------------------------------------------ call tracing (bisection)
struct TraceEnt { uint32_t pc, mask, a[4]; uint64_t mh; uint32_t d0, d1; };
static uint32_t g_dbg_addr = 0;
static void dbg_words(const uint8_t* ram, uint32_t& d0, uint32_t& d1) {
    d0 = d1 = 0; if (g_dbg_addr) { std::memcpy(&d0, ram + ((g_dbg_addr & 0x05FFFFFFu)), 4); std::memcpy(&d1, ram + ((g_dbg_addr & 0x05FFFFFFu)) + 4, 4); }
}
static bool g_tracemem = false;
static uint64_t hash_win(const uint8_t* ram) {
    // the heap arena and the caller's stack area; frames below the entry sp legitimately differ (promoted save slots)
    uint64_t h = 1469598103934665603ull;
    if (g_snap_mode) {
        for (auto& r : g_hash_ranges)
            for (uint32_t o = r.first; o + 8 <= r.second; o += 8) { uint64_t v; std::memcpy(&v, ram + o, 8); h ^= v; h *= 1099511628211ull; }
        return h;
    }
    const uint64_t* p = reinterpret_cast<const uint64_t*>(ram + (0x09000000u - 0x08000000u));
    for (size_t i = 0; i < (0x10000u / 8); ++i) { h ^= p[i]; h *= 1099511628211ull; }
    p = reinterpret_cast<const uint64_t*>(ram + (0x0904C000u - 0x08000000u));
    for (size_t i = 0; i < (0x4000u / 8); ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}
static std::vector<TraceEnt> g_otrace, g_ltrace;
static bool g_tracing = false;
extern "C" void lean_trace(uint32_t pc, uint32_t mask, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    if (g_tracing && g_ltrace.size() < 2000000) { uint32_t d0, d1; dbg_words(g_ram, d0, d1); g_ltrace.push_back({pc, mask, {a0, a1, a2, a3}, g_tracemem ? hash_win(g_ram) : 0, d0, d1}); }
}
static std::set<uint32_t> g_entry_set;

// ------------------------------------------------------------------------------------------ the oracle
static psprecomp::Runtime* g_rt;
static uint8_t* g_ip;                                  // interpreter's RAM

static void noop_fn(psprecomp::CorpusRuntime&, AllegrexContext&) {}

struct OracleResult { bool ok = false; const char* why = ""; uint64_t instructions = 0; };

static OracleResult run_oracle(const Trial& t, uint32_t entry, Cpu& out, uint64_t budget) {
    OracleResult res;
    AllegrexContext ctx{};
    cpu_to_ctx(t.cpu, ctx);
    ctx.pc = entry;
    uint32_t counter = 0;
    const uint64_t i0 = psprecomp::interpreter_stats().instructions;
    try {
        for (;;) {
            const uint64_t used = psprecomp::interpreter_stats().instructions - i0;
            if (used >= budget) { res.why = "budget"; break; }
            if (g_tracing && g_entry_set.count(ctx.pc))
                { uint32_t d0, d1; dbg_words(g_ip, d0, d1); g_otrace.push_back({ctx.pc, 0xF, {ctx.gpr[4], ctx.gpr[5], ctx.gpr[6], ctx.gpr[7]}, g_tracemem ? hash_win(g_ip) : 0, d0, d1}); }
            const auto ex = psprecomp::interpret_allegrex(*g_rt, ctx, g_tracing ? 1 : budget - used);
            if (ex == psprecomp::InterpreterExit::Dispatch) {
                if (ctx.pc == kSentinel) { res.ok = true; break; }
                bool stub = false;
                for (auto& s : kStubs) if (s.stub == ctx.pc) { stub = true; break; }
                if (!stub) { res.why = "dispatch to unknown function"; break; }
                uint32_t a[8]; float fa[8];
                for (int i = 0; i < 8; ++i) { a[i] = ctx.gpr[4 + i]; fa[i] = ctx.fpr[12 + i]; }
                if (getenv("LEAN_HLE_TRACE")) std::fprintf(stderr, "  orac HLE %08x n=%d a=%08x %08x %08x %08x sp=%08x ctr=%u\n", ctx.pc, stub_arity_gpr(ctx.pc), a[0], a[1], a[2], a[3], ctx.gpr[29], counter);
                fake_hle_writes(g_ip, ctx.pc, a, stub_arity_gpr(ctx.pc), counter);
                const FakeHle h = fake_hle(ctx.pc, a, fa, ctx.gpr[29], counter++, stub_arity_gpr(ctx.pc), stub_arity_fpr(ctx.pc));
                ctx.gpr[2] = h.v0; ctx.gpr[3] = h.v1; ctx.fpr[0] = h.f0;
                ctx.pc = ctx.gpr[31];
                if (ctx.pc == kSentinel) { res.ok = true; break; }
                continue;
            }
            if (ex == psprecomp::InterpreterExit::Budget) { if (g_tracing) continue; res.why = "budget"; break; }
            if (ex == psprecomp::InterpreterExit::Stopped) { res.why = "interpreter stopped"; if (getenv("LEAN_ORACLE_PC")) std::fprintf(stderr, "  oracle stopped at pc=%08x ra=%08x sp=%08x\n", ctx.pc, ctx.gpr[31], ctx.gpr[29]); break; }
            res.why = "unreachable";
            break;
        }
    } catch (const std::exception& e) {
        static std::set<std::string> pool; static int shown = 0;
        if (getenv("LEAN_SHOW_EXC") && shown++ < 60) std::fprintf(stderr, "exc entry=%08x pc=%08x: %s\n", entry, ctx.pc, e.what());
        std::string m = e.what(); const size_t p = m.find(" at 0x"); if (p != std::string::npos) m.resize(p);
        res.why = pool.insert("exception: " + m).first->c_str();
    } catch (...) {
        res.why = "interpreter exception";
    }
    res.instructions = psprecomp::interpreter_stats().instructions - i0;
    if (res.ok) ctx_to_cpu(ctx, out);
    return res;
}

// ------------------------------------------------------------------------------------------ lean side
struct LeanResult { bool ok = false; const char* why = ""; uint32_t pc = 0; };
static LeanResult run_lean(const Trial& t, uint32_t entry, Cpu& out) {
    LeanResult r;
    cpu_to_ctx(t.cpu, g_cpu);
    g_stub.hle_counter = 0; g_stub.depth = 0; g_stub.fatal = false; g_stub.abort_reason = nullptr;
    const uint32_t idx = (entry - LEAN_BASE) >> 2;
    g_in_lean = 1;
    if (sigsetjmp(g_stub.jmp, 1) == 0) {
        alarm(g_lean_alarm);
        g_lean_entry[idx]();
        alarm(0);
        g_in_lean = 0;
        r.ok = true;
        ctx_to_cpu(g_cpu, out);
    } else {
        alarm(0);
        g_in_lean = 0;
        r.why = g_stub.abort_reason ? g_stub.abort_reason : "abort";
        r.pc = g_stub.abort_pc;
    }
    return r;
}

// ------------------------------------------------------------------------------------------ compare
struct Diff { std::string text; std::string cls; };
static long g_nan_words = 0;
// did the oracle store into guest code?  (self-modifying code cannot be statically recompiled; the trial is not compared)
static bool wrote_code() {
    const uint32_t c0 = kCodeBegin - kRamBase, c1 = kCodeEnd - kRamBase;
    for (uint32_t k : g_side[0].dirty) {
        const uint32_t s0 = (uint32_t)(g_side[0].phase + (size_t)k * kPage);
        if (s0 >= c1 || s0 + kPage <= c0) continue;
        const uint32_t lo = std::max(s0, c0), hi = std::min(s0 + kPage, c1);
        if (std::memcmp(g_ip + lo, g_base + lo, hi - lo) != 0) return true;
    }
    return false;
}   // NaN-vs-NaN word differences that were not counted as mismatches

static void compare(const TestRow& row, const Trial& t, const Cpu& o, const Cpu& l, std::vector<Diff>& diffs) {
    char buf[256];
    for (int i = 0; i < 32; ++i) if (row.gpr_mask >> i & 1) {
        if (o.gpr[i] != l.gpr[i]) {
            std::snprintf(buf, sizeof buf, "r%d oracle=%08x lean=%08x", i, o.gpr[i], l.gpr[i]);
            diffs.push_back({buf, (row.sgpr_mask >> i & 1) ? "ret" : "ret-forced"});
        }
    }
    for (int i = 0; i < 32; ++i) if (row.fpr_mask >> i & 1) {
        if (!same_bits(o.fpr[i], l.fpr[i])) {
            const bool nan_both = o.fpr[i] != o.fpr[i] && l.fpr[i] != l.fpr[i];
            std::snprintf(buf, sizeof buf, "f%d oracle=%08x lean=%08x", i, std::bit_cast<uint32_t>(o.fpr[i]), std::bit_cast<uint32_t>(l.fpr[i]));
            if (nan_both && !g_strict_nan) { ++g_nan_words; continue; }
            diffs.push_back({buf, !(row.sfpr_mask >> i & 1) ? "ret-forced" : "ret"});
        }
    }
    for (int i = 0; i < 128; ++i) if (!same_bits(o.vfpu[i], l.vfpu[i])) {
        std::snprintf(buf, sizeof buf, "vfpu[%d] oracle=%08x lean=%08x", i, std::bit_cast<uint32_t>(o.vfpu[i]), std::bit_cast<uint32_t>(l.vfpu[i]));
        if (!g_strict_nan && o.vfpu[i] != o.vfpu[i] && l.vfpu[i] != l.vfpu[i]) { ++g_nan_words; continue; }
        diffs.push_back({buf, "vfpu"});
        if (diffs.size() > 8) break;
    }
    for (int i = 0; i < 16; ++i) if (o.ctrl[i] != l.ctrl[i]) {
        std::snprintf(buf, sizeof buf, "vfpu_ctrl[%d] oracle=%08x lean=%08x", i, o.ctrl[i], l.ctrl[i]);
        diffs.push_back({buf, "vfpu-ctrl"});
    }
    if ((o.fcr31 & ~(1u << 23)) != (l.fcr31 & ~(1u << 23))) {
        std::snprintf(buf, sizeof buf, "fcr31 oracle=%08x lean=%08x", o.fcr31, l.fcr31);
        diffs.push_back({buf, "fcr31"});
    }
    // memory: dirty pages of either side plus the always-compared ranges; the callee's dead frame below the entry sp
    // is not compared (lean keeps frame slots in locals)
    int nm = 0;
    auto cmp_range = [&](uint32_t off, uint32_t len, const char* where) {
        const uint8_t* a = g_ip + off; const uint8_t* b = g_ram + off;
        if (std::memcmp(a, b, len) == 0) return;
        for (uint32_t i = 0; i < len; ++i) if (a[i] != b[i]) {
            if (g_dead_hi != 0 && off + i >= g_dead_lo && off + i < g_dead_hi) continue;       // snapshot mode: the callee's dead frame
            // float words that are NaN on both sides differ only in sign/payload: the x86 hosts pick the propagated operand
            // by instruction order and ARM picks yet another one, and the guest FPU has its own rule; counted, not compared
            const uint32_t w = (off + i) & ~3u;
            uint32_t wa, wb; std::memcpy(&wa, g_ip + w, 4); std::memcpy(&wb, g_ram + w, 4);
            auto isnan32 = [](uint32_t x) { return (x & 0x7F800000u) == 0x7F800000u && (x & 0x007FFFFFu) != 0; };
            if (!g_strict_nan && isnan32(wa) && isnan32(wb)) { ++g_nan_words; i = w + 3 - off; continue; }
            if (nm++ < 4) {
                std::snprintf(buf, sizeof buf, "mem %s [%08x] oracle=%02x lean=%02x", where, kRamBase + off + i, a[i], b[i]);
                diffs.push_back({buf, "mem"});
            }
            break;
        }
    };
    for (Side& sd : g_side)
        for (uint32_t k : sd.dirty) {
            const uint32_t s0 = (uint32_t)(sd.phase + (size_t)k * kPage);
            if (s0 + kPage > kScratchOff && s0 < kScratchOff + kScratchBytes) continue;     // scratchpad: compared separately
            cmp_range(s0, kPage, "page");
        }
    cmp_range(kAlways[0][0], kAlways[0][1] - kAlways[0][0], "ram-first");
    cmp_range(kAlways[1][0], kAlways[1][1] - kAlways[1][0], "ram-last");
    cmp_range(kAlways[2][0], kStack - kRamBase - kAlways[2][0], "arena");
    cmp_range(kSp0 - kRamBase, kAlways[2][1] - (kSp0 - kRamBase), "stack");   // the dead frame below sp0 is not compared
}

// ------------------------------------------------------------------------------------------ uninitialised-stack probe
// A mismatch is explained when the *oracle itself* gives a different answer once the dead stack below the entry sp (which a
// function must not read before writing it) holds other bytes: the guest result depends on uninitialised memory.
struct Snap { Cpu c; std::vector<std::pair<uint32_t, std::vector<uint8_t>>> pages; std::vector<uint8_t> win; };
static void take_snap(Snap& s, const Cpu& c, const TestRow& row) {
    s.c = c;
    for (int i = 0; i < 32; ++i) { if (!(row.gpr_mask >> i & 1)) s.c.gpr[i] = 0; if (!(row.fpr_mask >> i & 1)) s.c.fpr[i] = 0.0f; }
    s.pages.clear();
    std::vector<uint32_t> ks = g_side[0].dirty; std::sort(ks.begin(), ks.end());
    for (uint32_t k : ks) {
        const uint32_t s0 = (uint32_t)(g_side[0].phase + (size_t)k * kPage);
        if (s0 + kPage > kScratchOff && s0 < kScratchOff + kScratchBytes) continue;
        s.pages.push_back({s0, std::vector<uint8_t>(g_ip + s0, g_ip + s0 + kPage)});
    }
    s.win.assign(g_ip + (kArena - kRamBase), g_ip + (kArena - kRamBase) + kArenaSize);
    s.win.insert(s.win.end(), g_ip + (kSp0 - kRamBase), g_ip + (kStackTop - kRamBase));
}
static bool snap_equal(const Snap& a, const Snap& b) {
    if (std::memcmp(a.c.gpr, b.c.gpr, sizeof a.c.gpr) || std::memcmp(a.c.fpr, b.c.fpr, sizeof a.c.fpr)) return false;
    if (a.win != b.win) return false;
    // pages: compare the union (a page missing on one side equals the pristine image there)
    std::map<uint32_t, const std::vector<uint8_t>*> m;
    for (auto& p : a.pages) m[p.first] = &p.second;
    for (auto& p : b.pages) {
        auto it = m.find(p.first);
        const uint8_t* other = it == m.end() ? g_base + p.first : it->second->data();
        if (std::memcmp(other, p.second.data(), kPage)) return false;
        if (it != m.end()) m.erase(it);
    }
    for (auto& kv : m) if (std::memcmp(kv.second->data(), g_base + kv.first, kPage)) return false;
    return true;
}

int main(int argc, char** argv) {
    int snap_limit = 1 << 30; int trials = 6, tries = 12; uint64_t seed = 12345; uint64_t budget = 60000; uint32_t only = 0; int only_trial = -1;
    int shard = 0, nshards = 1; bool verbose = false; int junk_probe = 1; const char* out_path = "mismatches.txt";
    uint32_t min_addr = 0, max_addr = 0xFFFFFFFFu; std::set<uint32_t> listed; bool trace = false; bool uninit_probe = true;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto nxt = [&]() { return std::string(argv[++i]); };
        if (a == "--trials") trials = std::stoi(nxt());
        else if (a == "--list") { std::ifstream lf(nxt()); std::string w; while (lf >> w) listed.insert((uint32_t)std::stoul(w, nullptr, 0)); }
        else if (a == "--dbg") g_dbg_addr = (uint32_t)std::stoul(nxt(), nullptr, 0);
        else if (a == "--no-uninit-probe") uninit_probe = false;
        else if (a == "--trace") trace = true;
        else if (a == "--special") g_special = true;
        else if (a == "--hooks") g_hooks = true;
        else if (a == "--snap") g_snap_mode = true, g_snap_dir = nxt();
        else if (a == "--snap-limit") snap_limit = std::stoi(nxt());
        else if (a == "--stats") g_stats_file = std::fopen(nxt().c_str(), "w");
        else if (a == "--strict-nan") g_strict_nan = true;
        else if (a == "--tracemem") { trace = true; g_tracemem = true; }
        else if (a == "--tries") tries = std::stoi(nxt());
        else if (a == "--seed") seed = std::stoull(nxt());
        else if (a == "--budget") budget = std::stoull(nxt());
        else if (a == "--only") only = (uint32_t)std::stoul(nxt(), nullptr, 0);
        else if (a == "--trial") only_trial = std::stoi(nxt());
        else if (a == "--shard") { std::string s = nxt(); shard = std::stoi(s.substr(0, s.find('/'))); nshards = std::stoi(s.substr(s.find('/') + 1)); }
        else if (a == "--verbose") verbose = true;
        else if (a == "--no-junk-probe") junk_probe = 0;
        else if (a == "--out") out_path = argv[++i];
        else if (a == "--range") { min_addr = (uint32_t)std::stoul(nxt(), nullptr, 0); max_addr = (uint32_t)std::stoul(nxt(), nullptr, 0); }
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    if (g_snap_mode) {
        DIR* d = opendir(g_snap_dir.c_str());
        if (!d) { std::fprintf(stderr, "cannot open %s\n", g_snap_dir.c_str()); return 2; }
        std::vector<std::string> names;
        while (dirent* e = readdir(d)) { std::string n = e->d_name; if (n.size() > 5 && n.compare(n.size() - 5, 5, ".snap") == 0) names.push_back(n); }
        closedir(d);
        std::sort(names.begin(), names.end());
        for (auto& n : names) {
            const uint32_t entry = (uint32_t)std::stoul(n.substr(0, n.find('_')), nullptr, 16);
            auto& v = g_snap_files[entry];
            if ((int)v.size() < snap_limit) v.push_back(g_snap_dir + "/" + n);
        }
        tries = 1; junk_probe = 0; g_lean_alarm = 30;
        if (budget == 60000) budget = 30000000;
    }
    // image
    std::vector<uint8_t> image;
    { std::ifstream f(getenv("LEAN_IMAGE") ? getenv("LEAN_IMAGE") : "/work/infected/work/image.bin", std::ios::binary); image.assign(std::istreambuf_iterator<char>(f), {}); }
    if (image.empty()) { std::fprintf(stderr, "image not found\n"); return 2; }
    g_image_end = 0x08804000u + (uint32_t)image.size();
    for (auto& r : kRows) { g_entries.push_back(r.addr); g_entry_set.insert(r.addr); }

    psprecomp::Runtime rt(kRamSize);
    g_rt = &rt;
    rt.register_function(kSentinel, &noop_fn, "sentinel");
    for (auto& s : kStubs) rt.register_function(s.stub, &noop_fn, "stub");
    g_ip = rt.memory().raw_pointer(kRamBase, kRamSize);
    if (!g_ip) { std::fprintf(stderr, "no raw RAM pointer\n"); return 2; }
    g_base = static_cast<uint8_t*>(std::calloc(kRamSize, 1));
    std::memcpy(g_base + 0x804000, image.data(), image.size());
    std::memcpy(g_ip, g_base, kRamSize);
    std::memcpy(g_ram, g_base, kRamSize);
    g_ivram = const_cast<uint8_t*>(rt.memory().vram_bytes().data());
    setup_side(g_side[0], g_ip);
    setup_side(g_side[1], g_ram);
    protect_vram();
    struct sigaction sa{}; sa.sa_sigaction = on_segv; sa.sa_flags = SA_SIGINFO | SA_NODEFER; sigemptyset(&sa.sa_mask);
    for (int sg : {SIGSEGV, SIGBUS, SIGFPE}) sigaction(sg, &sa, nullptr);
    struct sigaction sb{}; sb.sa_handler = on_alarm; sb.sa_flags = SA_NODEFER; sigemptyset(&sb.sa_mask); sigaction(SIGALRM, &sb, nullptr);

    FILE* mf = std::fopen(out_path, "w");
    long n_selfmod = 0, n_nonfn = 0, n_uninit = 0, n_forced = 0, n_mirror = 0, n_trials = 0, n_cmp = 0, n_eq = 0, n_mis = 0, n_skip = 0, n_leanabort = 0, n_junk = 0;
    std::map<std::string, long> by_class, skip_why;
    int funcs = 0, funcs_cmp = 0, funcs_bad = 0, funcs_junk = 0;
    std::vector<uint32_t> bad_funcs, uncovered;
    long oracle_insns = 0;
    auto t_start = std::chrono::steady_clock::now();
    int row_index = -1;
    for (const TestRow& row : kRows) {
        ++row_index;
        if (only && row.addr != only) continue;
        if (!listed.empty() && !listed.count(row.addr)) continue;
        if (row.addr < min_addr || row.addr >= max_addr) continue;
        if ((row_index % nshards) != shard && !only && listed.empty()) continue;
        ++funcs;
        int cmp_this = 0, bad_this = 0; bool junk_dep = false;
        const int ntr = g_snap_mode ? (int)g_snap_files[row.addr].size() : trials;
        for (int tr = 0; tr < ntr; ++tr) {
            if (only_trial >= 0 && tr != only_trial) continue;
            ++n_trials;
            bool done = false;
            for (int att = 0; att < tries && !done; ++att) {
            Gen g; g.rng.seed(seed * 1000003ull + row.addr * 2654435761ull + tr + 7919ull * att);
            const bool junk = junk_probe && (tr % 3 == 2);
            g.mode = (int)((tr + att + (row.addr >> 2)) % 3);
            Trial t;
            if (g_snap_mode) { if (!load_snapshot(t, g_snap_files[row.addr][(size_t)tr])) { ++n_skip; ++skip_why["snapshot unreadable"]; continue; } }
            else { make_trial(t, g, junk); if (g_hooks) hook_case(t, g, row.addr, tr); }
            install_trial(t);
            g_tracing = trace; g_otrace.clear(); g_ltrace.clear();
            Cpu oc{};
            const OracleResult orr = run_oracle(t, row.addr, oc, budget);
            oracle_insns += (long)orr.instructions;
            if (!orr.ok) { ++n_skip; ++skip_why[orr.why]; restore_dirty(); special_compare_reset(nullptr); continue; }
            if (wrote_code()) { ++n_selfmod; restore_dirty(); special_compare_reset(nullptr); continue; }
            std::vector<uint8_t> oracle_mem;                       // not needed: compare in place
            // lean runs on its own memory (identical start)
            Cpu lc{};
            const LeanResult lr = run_lean(t, row.addr, lc);
            std::vector<Diff> diffs;
            if (!lr.ok && std::strcmp(lr.why, "indirect call to a non-function") == 0) {
                // the target is not a function entry of the corpus (random pointers in the arena); in production the runtime
                // interprets unclaimed code (ARCHITECTURE.md), which is outside what this harness compares
                ++n_nonfn; special_compare_reset(nullptr); restore_dirty(); done = true; continue;
            }
            if (!lr.ok && std::strcmp(lr.why, "unmodelled arena region") == 0) {
                ++n_mirror; special_compare_reset(nullptr); restore_dirty(); done = true; continue;
            }
            if (!lr.ok) {
                char buf[200]; std::snprintf(buf, sizeof buf, "lean aborted: %s (pc/addr %08x)", lr.why, lr.pc);
                diffs.push_back({buf, std::string("lean-abort:") + lr.why});
                ++n_leanabort;
            } else {
                compare(row, t, oc, lc, diffs);
                for (size_t i = 0; i < diffs.size();)
                    if (diffs[i].cls == "ret-forced") { ++n_forced; diffs.erase(diffs.begin() + (long)i); } else ++i;
                std::vector<std::string> sp;
                special_compare_reset(&sp);
                for (auto& x : sp) diffs.push_back({x, "mem-special"});
            }
            if (!lr.ok) special_compare_reset(nullptr);
            ++n_cmp; ++cmp_this;
            if (!diffs.empty() && lr.ok && uninit_probe) {
                Snap s1; take_snap(s1, oc, row);
                restore_dirty();
                Trial t2 = t; const bool was_tracing = g_tracing; g_tracing = false;
                { std::mt19937_64 gr(seed ^ 0xDEADBEEFull ^ row.addr);
                  uint32_t* w = reinterpret_cast<uint32_t*>(t2.win.data()) + (kStack - kArena) / 4;
                  for (uint32_t i = 0; i < (kSp0 - kStack) / 4; ++i) w[i] = (uint32_t)gr();
                  if (g_snap_mode)                                       // the recorded thread's dead frame (below the entry sp) gets other junk
                      for (SnapPage& pg : t2.pages)
                          for (uint32_t o = 0; o + 4 <= pg.data.size(); o += 4) {
                              const uint32_t ro = pg.off + o;
                              if (ro >= g_dead_lo && ro < g_dead_hi) { const uint32_t v = (uint32_t)gr(); std::memcpy(pg.data.data() + o, &v, 4); }
                          }
                  // registers outside the calling contract get other values too (hidden inputs show up as oracle differences)
                  for (int i = 1; i < 32; ++i) if (i != 28 && i != 29 && i != 31 && !(row.pgpr_mask >> i & 1)) t2.cpu.gpr[i] = (uint32_t)gr();
                  for (int i = 0; i < 32; ++i) if (!(row.pfpr_mask >> i & 1)) t2.cpu.fpr[i] = (float)((int32_t)(gr() % 20001) - 10000) / 64.0f;
                  t2.cpu.hi = (uint32_t)gr(); t2.cpu.lo = (uint32_t)gr(); }
                install_trial(t2);
                Cpu oc2{};
                const OracleResult o2 = run_oracle(t2, row.addr, oc2, budget);
                if (o2.ok) {
                    Snap s2; take_snap(s2, oc2, row);
                    if (!snap_equal(s1, s2)) { ++n_uninit; diffs.clear(); }
                }
                g_tracing = was_tracing;
                special_compare_reset(nullptr);
            }
            if (diffs.empty()) ++n_eq;
            else {
                ++n_mis; ++bad_this;
                if (junk) for (auto& d : diffs) d.cls = "junk:" + d.cls;
                if (junk) junk_dep = true;
                for (auto& d : diffs) ++by_class[d.cls];
                if (mf) {
                    std::fprintf(mf, "%08x trial %d%s:", row.addr, tr, junk ? " (junk regs)" : "");
                    for (size_t i = 0; i < diffs.size() && i < 6; ++i) std::fprintf(mf, " | [%s] %s", diffs[i].cls.c_str(), diffs[i].text.c_str());
                    std::fprintf(mf, "\n");
                }
                if (verbose || only) {
                    std::printf("MISMATCH %08x trial %d (oracle %llu insns)\n", row.addr, tr, (unsigned long long)orr.instructions);
                    for (int i = 4; i < 12; ++i) std::printf("  a%d=%08x", i - 4, t.cpu.gpr[i]);
                    std::printf("\n");
                    for (auto& d : diffs) std::printf("  [%s] %s\n", d.cls.c_str(), d.text.c_str());
                    if (only) {
                        std::printf("  initial vfpu:");
                        for (int i = 0; i < 128; ++i) std::printf("%s%08x", i % 16 ? " " : "\n    ", std::bit_cast<uint32_t>(t.cpu.vfpu[i]));
                        std::printf("\n  initial ctrl[3]=%08x\n  final oracle vfpu:", t.cpu.ctrl[3]);
                        for (int i = 0; i < 128; ++i) std::printf("%s%08x", i % 16 ? " " : "\n    ", std::bit_cast<uint32_t>(oc.vfpu[i]));
                        std::printf("\n  final lean vfpu:");
                        for (int i = 0; i < 128; ++i) std::printf("%s%08x", i % 16 ? " " : "\n    ", std::bit_cast<uint32_t>(lc.vfpu[i]));
                        std::printf("\n");
                    }
                }
            }
            if (trace && !diffs.empty()) {
                size_t n = std::min(g_otrace.size(), g_ltrace.size()), k = 0;
                for (; k < n; ++k) {
                    const TraceEnt &o = g_otrace[k], &l = g_ltrace[k];
                    bool same = o.pc == l.pc && (!g_tracemem || o.mh == l.mh);
                    for (int i = 0; i < 4 && same && !g_tracemem; ++i) if ((l.mask >> i & 1) && o.a[i] != l.a[i]) same = false;
                    if (!same) break;
                }
                std::printf("  trace: oracle %zu entries, lean %zu entries, first divergence at %zu\n", g_otrace.size(), g_ltrace.size(), k);
                for (size_t j = k > 4 ? k - 4 : 0; j < k + 2; ++j) {
                    if (j < g_otrace.size()) std::printf("    oracle[%zu] %08x a0-3 %08x %08x %08x %08x\n", j, g_otrace[j].pc, g_otrace[j].a[0], g_otrace[j].a[1], g_otrace[j].a[2], g_otrace[j].a[3]);
                    if (g_dbg_addr && j < g_otrace.size()) std::printf("      oracle mem[%08x] = %08x %08x\n", g_dbg_addr, g_otrace[j].d0, g_otrace[j].d1);
                    if (j < g_ltrace.size()) std::printf("    lean  [%zu] %08x a0-3 %08x %08x %08x %08x (mask %x)\n", j, g_ltrace[j].pc, g_ltrace[j].a[0], g_ltrace[j].a[1], g_ltrace[j].a[2], g_ltrace[j].a[3], g_ltrace[j].mask);
                    if (g_dbg_addr && j < g_ltrace.size()) std::printf("      lean   mem[%08x] = %08x %08x\n", g_dbg_addr, g_ltrace[j].d0, g_ltrace[j].d1);
                }
            }
            g_tracing = false;
            // restore both memories (page tracking + windows are rewritten at the next trial)
            restore_dirty();
            done = true;
            }
        }
        if (cmp_this) ++funcs_cmp; else uncovered.push_back(row.addr);
        if (g_stats_file) std::fprintf(g_stats_file, "%08x %d %d\n", row.addr, cmp_this, bad_this);
        if (bad_this) { ++funcs_bad; bad_funcs.push_back(row.addr); }
        if (junk_dep) ++funcs_junk;
        if (funcs % 500 == 0) {
            double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
            std::fprintf(stderr, "  %d functions, %.0fs, compared %ld mismatches %ld\n", funcs, el, n_cmp, n_mis);
        }
    }
    double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
    std::printf("functions=%d trials/function=%d total_trials=%ld  (%.0fs, oracle instructions %ld)\n", funcs, trials, n_trials, el, oracle_insns);
    std::printf("compared=%ld equal=%ld MISMATCH=%ld (of which lean aborted=%ld)  skipped(oracle could not finish)=%ld\n", n_cmp, n_eq, n_mis, n_leanabort, n_skip);
    std::printf("functions with >=1 compared trial=%d / %d, functions with a mismatch=%d, functions whose oracle result depends on junk registers=%d\n",
                funcs_cmp, funcs, funcs_bad, funcs_junk);
    std::printf("  explained: jalr to a non-function-entry target (production: interpreter fallback), trial not compared: %ld\n", n_nonfn);
    std::printf("  explained: oracle result depends on uninitialised stack contents or registers outside the calling contract (second oracle run differs): %ld\n", n_uninit);
    std::printf("  explained: v0/v1/f0 differences in registers no strong reader depends on (jalr-forced pass-through junk): %ld\n", n_forced);
    std::printf("  explained: NaN-vs-NaN word differences (sign/payload only; operand-order NaN propagation), words not compared: %ld\n", g_nan_words);
    std::printf("  oracle stored into guest code (self-modifying; trial not compared): %ld\n", n_selfmod);
    std::printf("  lean touched an unmodelled arena region (VRAM mirror / hole), trial not compared: %ld\n", n_mirror);
    for (auto& kv : by_class) std::printf("  mismatch class %-28s %ld\n", kv.first.c_str(), kv.second);
    for (auto& kv : skip_why) std::printf("  skipped: %-34s %ld\n", kv.first.c_str(), kv.second);
    for (size_t i = 0; i < bad_funcs.size() && i < 40; ++i) std::printf("  bad 0x%08X\n", bad_funcs[i]);
    if (mf) std::fclose(mf);
    {
        FILE* uf = std::fopen((std::string(out_path) + ".uncovered").c_str(), "w");
        if (uf) { for (uint32_t a : uncovered) std::fprintf(uf, "0x%08x\n", a); std::fclose(uf); }
    }
    return n_mis ? 1 : 0;
}
