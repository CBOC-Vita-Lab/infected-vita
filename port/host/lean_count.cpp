// Entry counter for a lean corpus built with -DLEAN_TRACE (recomp/build_lib.sh with LEAN_EXTRA=-DLEAN_TRACE): the generated
// code calls lean_trace() at the entry of every function. This counts them, so the bench window can say how many function
// entries a flip makes, how many of them came through pk_guest_call (indirect calls and runtime-to-guest calls) and which
// functions are entered most. A corpus built without -DLEAN_TRACE never calls it and nothing is printed.
#include "perf/bench.hpp"
#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/exec.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

constexpr std::uint32_t kBase = 0x08804000u;
constexpr std::size_t kSlots = 1u << 20;
std::uint32_t g_counts[kSlots];
std::uint64_t g_total = 0;
bool g_seen = false;

std::vector<std::uint32_t> g_begin_counts;
std::uint64_t g_begin_total = 0;
psprecomp::ExecStats g_begin_exec{};

void window_begin() {
    if (!g_seen) return;
    g_begin_counts.assign(g_counts, g_counts + kSlots);
    g_begin_total = g_total;
    g_begin_exec = psprecomp::exec_stats();
}

// INFECTED_BENCH_UBENCH=1: what one pk_guest_call costs, as a round trip to a ten-instruction leaf of the game (0x08A24A98, a bit
// scan of a0 that touches no memory) through the runtime's classification and the entry thunk. An upper bound of the dispatch
// overhead of an indirect call; the work in the leaf itself is a few ns.
void ubench() {
    if (std::getenv("INFECTED_BENCH_UBENCH") == nullptr || !psprecomp::exec_have_lean_table()) return;
    constexpr std::uint32_t kLeaf = 0x08A24A98u;
    if (psprecomp::exec_classify(kLeaf) != psprecomp::TargetKind::Lean) return;
    const psprecomp::AllegrexContext saved = g_cpu;
    constexpr int kRounds = 5'000'000;
    const auto start = std::chrono::steady_clock::now();
    std::uint32_t sink = 0;
    for (int i = 0; i < kRounds; ++i) {
        g_cpu.gpr[4] = 0x200u + static_cast<std::uint32_t>(i & 0xFF);
        pk_guest_call(kLeaf);
        sink += g_cpu.gpr[2];
    }
    const double ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
    g_cpu = saved;
    portablekit::bench::out("[bench] pk_guest_call round trip to a ten-instruction leaf: %.1f ns (check %u)\n", ns / kRounds, sink & 1u);
}

void window_end(std::uint64_t flips) {
    if (!g_seen || flips == 0u || g_begin_counts.empty()) {
        ubench();
        return;
    }
    const double n = static_cast<double>(flips);
    const std::uint64_t total = g_total - g_begin_total;
    const psprecomp::ExecStats now = psprecomp::exec_stats();
    const std::uint64_t indirect = now.lean_calls - g_begin_exec.lean_calls;
    portablekit::bench::out("[bench] lean function entries per flip: %.1f, of which through pk_guest_call %.1f (%.1f%%), direct C calls %.1f\n",
                static_cast<double>(total) / n, static_cast<double>(indirect) / n,
                total != 0u ? 100.0 * static_cast<double>(indirect) / static_cast<double>(total) : 0.0,
                static_cast<double>(total - std::min(total, indirect)) / n);
    struct Row { std::uint32_t pc; std::uint32_t calls; };
    std::vector<Row> rows;
    for (std::size_t i = 0; i < kSlots; ++i) {
        const std::uint32_t d = g_counts[i] - g_begin_counts[i];
        if (d != 0u) rows.push_back({kBase + static_cast<std::uint32_t>(i) * 4u, d});
    }
    std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.calls > b.calls; });
    portablekit::bench::out("[bench] distinct functions entered in the window: %llu; most entered (calls per flip):\n", static_cast<unsigned long long>(rows.size()));
    for (std::size_t i = 0; i < rows.size() && i < 25u; ++i)
        portablekit::bench::out("[bench]   0x%08X %9.1f\n", rows[i].pc, static_cast<double>(rows[i].calls) / n);
    ubench();
}

struct Register {
    Register() { portablekit::bench::add_window_hooks(&window_begin, &window_end); }
} g_register;

} // namespace

extern "C" void lean_trace(std::uint32_t pc, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) {
    g_seen = true;
    ++g_total;
    const std::uint32_t slot = (pc - kBase) >> 2;
    if (slot < kSlots) ++g_counts[slot];
}
