#include "infected_tuning.hpp"
#include "infected_bot.hpp"

#include "profile.hpp"
#include "settings/port_settings.hpp"
#include "psprecomp/corpus_abi.hpp"
#include "psprecomp/exec.hpp"
#include "psprecomp/runtime.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <chrono>
#include <cmath>
#include <vector>

namespace psprecomp {
void set_write_watch(std::uint32_t address, std::uint32_t size);
}

namespace infected {
namespace {

using psprecomp::Runtime;

float as_float(std::uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

} // namespace

// The ONE place the two gameplay settings are read: the port settings (host/settings/port_settings.hpp), where the developer override
// (INFECTED_PERF_ENEMY_DENSITY, config.txt's perf.enemy_density) and the player's settings.txt are already layered. The values are
// the percentages of the allowed lists (100 / 125 / 150 / 200 and 100 / 150 / 200). `fallback` is for a key this module does not own.
int tuning_setting(portablekit::port::Key key, int fallback) {
    namespace port = portablekit::port;
    switch (key) {
    case port::Key::PerfEnemyDensity: return static_cast<int>(port::enemy_density_percent());
    case port::Key::PerfViewDistance: return static_cast<int>(port::view_distance_percent());
    default: break;
    }
    return fallback;
}

void install_kill_hook(Runtime &runtime);  // below, with the developer harness

// The patch point of the game profile (the executable is in memory, nothing has run yet): the two overlay rows are usable from here on
// (the framework lists them as "not available yet" for a program without this module, such as the replayer).
void tuning_install(Runtime &runtime) {
    namespace port = portablekit::port;
    port::settings().set_unavailable(port::Key::PerfEnemyDensity, nullptr);
    port::settings().set_unavailable(port::Key::PerfViewDistance, nullptr);
    std::fprintf(stderr, "[tune] gameplay settings: perf.enemy_density and perf.view_distance are usable in the menu\n");
    install_kill_hook(runtime);
    bot::install(runtime);
}

// ---- view distance ---------------------------------------------------------------------------------------------------------
//
// What bounds the view (docs/GAMEPLAY_TUNING.md): the world renderer (CWorldRender, singleton pointer at 0x08AD0450) owns the
// environment of the cell the player is in: three records {name, near, far, fog_start, colour, fade_seconds} at +0x68 (from),
// +0x80 (to) and +0x98 (current, blended over `fade_seconds`). Every frame 0x0886E020 sets the world camera's near plane,
// far plane and fog start from the CURRENT record, so far is both the clip distance and the end of the fog ramp.
// The values come from the CellVolume entity of the cell (a record at +0xEC of the object, copied in by 0x0886F5DC when the player
// enters it) and the level data never changes them afterwards. We scale far and fog_start of those CellVolume records, which is
// exactly what the game then copies and blends, and fix up the three live records when the setting changes at run time.
namespace view {

constexpr std::uint32_t kRamLo = 0x08800000u, kRamHi = 0x0A000000u;
constexpr std::uint32_t kWorldSingleton = 0x08AD0450u;  // CWorldRender*, 0 between levels
constexpr std::uint32_t kCellVtableA = 0x08AD25F0u;     // CellVolume: *(obj+0x04)
constexpr std::uint32_t kCellVtableB = 0x08AD2610u;     // and *(obj+0x1C)
constexpr std::uint32_t kRecord = 0xECu;                // environment record inside a CellVolume
constexpr std::uint32_t kSetOffsets[3] = {0x68u, 0x80u, 0x98u};
// record field offsets
constexpr std::uint32_t kName = 0, kNear = 4, kFar = 8, kFog = 12;

struct Cell {
    float orig_far, orig_fog;
    std::uint32_t written_far, written_fog;  // bits we last stored (to see the game re-initialise the record)
};

struct State {
    std::unordered_map<std::uint32_t, Cell> cells;
    std::uint32_t world = 0;
    std::uint32_t level_serial = 0;  // counts the changes of the world renderer (a level load, a retry): density::frame latches its setting by it
    std::uint64_t scan_due = 0;
    std::uint64_t world_since = 0;
    std::uint32_t empty_scans = 0;
    std::uint64_t next_poll = 0;
    float applied = 1.0f;  // scale the cell records and the live records carry now
    float wanted = 1.0f;
    bool verbose = std::getenv("INFECTED_TUNE_LOG") != nullptr;
};

State &state() {
    static State s;
    return s;
}

std::uint32_t bits_of(float f) {
    std::uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}

bool plausible(float f) { return f > 0.0f && f < 1.0e6f; }

bool in_ram(std::uint32_t a) { return a >= kRamLo && a < kRamHi - 0x400u; }

void apply_record(Runtime &runtime, std::uint32_t base, Cell &c, float k) {
    auto &mem = runtime.memory();
    c.written_far = bits_of(c.orig_far * k);
    c.written_fog = bits_of(c.orig_fog * k);
    mem.store32(base + kRecord + kFar, c.written_far);
    mem.store32(base + kRecord + kFog, c.written_fog);
}

// Every CellVolume in guest RAM: both vtable words and a sane record.
void scan_cells(Runtime &runtime, std::uint64_t flip) {
    State &s = state();
    auto &mem = runtime.memory();
    int added = 0;
    // the arena is little endian and the vtable word is checked first: a plain word loop over the 24 MB (a few milliseconds)
    const auto *words = reinterpret_cast<const std::uint32_t *>(PK_GUEST_TO_HOST(kRamLo));
    for (std::uint32_t a = kRamLo; a < kRamHi - 0x200u; a += 4u) {
        if (words[((a - kRamLo) >> 2) + 1u] != kCellVtableA) continue;
        if (mem.load32(a + 4u) != kCellVtableA) continue;
        if (mem.load32(a + 0x1Cu) != kCellVtableB) continue;
        const float far_plane = as_float(mem.load32(a + kRecord + kFar)), fog = as_float(mem.load32(a + kRecord + kFog));
        if (!plausible(far_plane) || !plausible(fog)) continue;
        auto it = s.cells.find(a);
        if (it != s.cells.end()) continue;
        Cell c{far_plane, fog, 0u, 0u};
        s.cells.emplace(a, c);
        ++added;
        if (s.verbose)
            std::fprintf(stderr, "[tune] flip %llu CellVolume 0x%08x near=%g far=%g fog=%g colour=%08x fade=%g name=%08x\n",
                         static_cast<unsigned long long>(flip), a, as_float(mem.load32(a + kRecord + kNear)), far_plane, fog,
                         mem.load32(a + kRecord + 0x10u), as_float(mem.load32(a + kRecord + 0x14u)), mem.load32(a + kRecord + kName));
    }
    if (s.verbose) std::fprintf(stderr, "[tune] scan at flip %llu: %d new CellVolumes, %u known\n", static_cast<unsigned long long>(flip), added, static_cast<unsigned>(s.cells.size()));
}

// The live records hold values derived from the cell records: scale them by `ratio` (a change of the setting) or, for a
// record that was copied before we had scaled its cell, snap it to the scaled value.
void fix_live_records(Runtime &runtime, float ratio, bool match_by_name, float k) {
    State &s = state();
    auto &mem = runtime.memory();
    if (!in_ram(s.world)) return;
    for (std::uint32_t off : kSetOffsets) {
        const std::uint32_t set = s.world + off;
        const std::uint32_t name = mem.load32(set + kName);
        if (name == 0u) continue;  // never initialised: the game snaps all three to the first cell it enters
        const float far_plane = as_float(mem.load32(set + kFar)), fog = as_float(mem.load32(set + kFog));
        if (!plausible(far_plane) || !plausible(fog)) continue;
        if (match_by_name) {
            for (auto &[base, c] : s.cells) {
                if (mem.load32(base + kRecord + kName) != name) continue;
                if (bits_of(far_plane) == bits_of(c.orig_far) && bits_of(fog) == bits_of(c.orig_fog) && k != 1.0f) {
                    mem.store32(set + kFar, bits_of(c.orig_far * k));
                    mem.store32(set + kFog, bits_of(c.orig_fog * k));
                }
                break;
            }
        } else {
            mem.store32(set + kFar, bits_of(far_plane * ratio));
            mem.store32(set + kFog, bits_of(fog * ratio));
        }
    }
}

void frame(Runtime &runtime, std::uint64_t flip) {
    State &s = state();
    auto &mem = runtime.memory();
    if (flip >= s.next_poll) {
        s.next_poll = flip + 30u;
        const int percent = std::clamp(tuning_setting(portablekit::port::Key::PerfViewDistance, 100), 100, 200);
        s.wanted = static_cast<float>(percent) / 100.0f;
        if (const char *raw = std::getenv("INFECTED_TUNE_VIEW_RAW")) s.wanted = static_cast<float>(std::atof(raw));  // experiments: unclamped factor
    }
    const std::uint32_t world = mem.load32(kWorldSingleton);
    if (world != s.world) {
        s.world = world;
        ++s.level_serial;
        s.cells.clear();
        s.applied = 1.0f;  // a fresh level: the live records are the game's own
        s.scan_due = flip + 1u;
        s.world_since = flip;
        s.empty_scans = 0u;
        if (s.verbose) std::fprintf(stderr, "[tune] flip %llu world renderer 0x%08x\n", static_cast<unsigned long long>(flip), world);
    }
    if (world == 0u) return;
    if (flip >= s.scan_due) {
        const std::size_t before = s.cells.size();
        const auto t0 = std::chrono::steady_clock::now();
        scan_cells(runtime, flip);
        if (s.verbose) std::fprintf(stderr, "[tune] scan took %.2f ms\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        // a level creates its CellVolumes while it loads: look again a few times, then now and then
        s.scan_due = flip + std::clamp<std::uint64_t>(flip - s.world_since, 30u, 900u);
        // a scan walks the whole 24 MB (about 10 ms on this host, more on a Vita): the CellVolumes of a level all exist within a second of its renderer, so
        // the scans stop once two in a row find nothing new (3 or 4 per level; a retry builds a new renderer and starts again), at the latest after 900 flips
        if (flip - s.world_since > 900u) s.scan_due = ~0ull;
        s.empty_scans = (s.cells.size() != before) ? 0u : s.empty_scans + 1u;
        if (!s.cells.empty() && s.empty_scans >= 2u) s.scan_due = ~0ull;  // found some, and two scans in a row found no more: the level is loaded
        if (s.cells.size() != before || s.wanted != s.applied) {
            for (auto &[base, c] : s.cells)
                if (c.written_far == 0u) apply_record(runtime, base, c, s.wanted);
            fix_live_records(runtime, 1.0f, true, s.wanted);
            s.applied = s.wanted;
        }
    }
    // the game re-initialises a record (a level restart that keeps the object): take the new original
    if (flip % 30u == 0u) {
        for (auto &[base, c] : s.cells) {
            if (mem.load32(base + kRecord + kFar) != c.written_far || mem.load32(base + kRecord + kFog) != c.written_fog) {
                c.orig_far = as_float(mem.load32(base + kRecord + kFar));
                c.orig_fog = as_float(mem.load32(base + kRecord + kFog));
                apply_record(runtime, base, c, s.wanted);
            }
        }
    }
    if (s.wanted != s.applied && !s.cells.empty()) {
        for (auto &[base, c] : s.cells) apply_record(runtime, base, c, s.wanted);
        fix_live_records(runtime, s.wanted / s.applied, false, s.wanted);
        if (s.verbose) std::fprintf(stderr, "[tune] flip %llu view distance %g%% -> %g%%\n", static_cast<unsigned long long>(flip), s.applied * 100.0, s.wanted * 100.0);
        s.applied = s.wanted;
    }
}

} // namespace view

// ---- enemy density -----------------------------------------------------------------------------------------------------------
//
// What bounds the number of enemies (docs/GAMEPLAY_TUNING.md): every level has one CCell object per cell (class creator 0x08A552B4, ctor
// 0x088BCB0C, 0x240 bytes; the cell manager singleton at 0x08ACDD18 keeps their pointers in a vector at +0x28/+0x2C). A cell owns its
// CNpcGen generators (array at +0x3C, count at +0x64) and two lists of the NPCs that belong to it (+0x170[1], +0x170[2]; their sizes are
// read by 0x088BCCD0 / 0x088BCCF0). The cell's tick (0x088BD9DC) asks a generator for an NPC (0x088BD074 -> CNpcGen::Spawn 0x08819C34)
// only while  alive(cell) < cell+0x98  (the level's "maximum infected in the cell"; 4 in mission 1, 15 in most late cells) and the spawn
// timers cell+0x184 / +0x18C are negative; after a spawn the next one waits lerp(cell+0xDC, cell+0xE0, random) seconds. Those three
// numbers are the whole spawn rate and cap; the generators' remaining counts (+0x11C) are the supply and stay as they are, so a level
// still asks for the same number of kills.
// Behind the cap are fixed-size pools (alloc ids at 0x09EAC5E4): 20 NPCs and 105 ghosts, plus the pools that grow with each NPC (weapons,
// pickups, projectiles, particles, collidables, graphical assets). They are sized once at start-up by 0x08892BF0 and an exhausted pool
// returns 0 to its caller, so when the setting is above 100 at start-up the pools are rebuilt twice as large (pool_term 0x0889718C +
// pool_init 0x0889700C, nothing is in use yet) and the cap is held below the free room of the NPC and ghost pools at all times.
namespace density {

constexpr std::uint32_t kRegistry = 0x09EAC5E4u;      // allocator pointer per id
constexpr std::uint32_t kPoolInit = 0x0889700Cu;      // (pool, element size, count, align)
constexpr std::uint32_t kPoolTerm = 0x0889718Cu;      // (pool): frees the storage and the free list
constexpr std::uint32_t kCellManager = 0x08ACDD18u;   // pointer to the cell manager; vector of CCell* at +0x28 .. +0x2C
constexpr std::uint32_t kCellVtable = 0x08AD2570u;    // *(cell + 4)
constexpr std::uint32_t kCellMax = 0x98u, kCellListMax = 0xA4u, kCellGapA = 0xDCu, kCellGapB = 0xE0u, kCellLists = 0x170u;
constexpr std::uint32_t kPoolElem = 0x64u, kPoolCount = 0x68u, kPoolUsed = 0x6Cu, kPoolState = 0x74u, kPoolStorage = 0x78u, kPoolFree = 0x7Cu;

struct PoolDef {
    int id;
    const char *name;
    std::uint32_t element;  // the arguments of the game's own start-up table
    std::uint32_t count;
};
constexpr PoolDef kPools[] = {{1, "Npc", 0x540u, 20u},       {2, "NpcGhost", 0xD0u, 105u},     {3, "Pickup", 0x1C0u, 50u},
                              {4, "Projectile", 0x1ACu, 50u}, {5, "Weapon", 0x5ACu, 80u},       {6, "FxSimple", 0x150u, 128u},
                              {7, "ACollidable", 0x18u, 200u}, {8, "VoiceHandle", 0x8u, 50u},   {17, "GraphicalAsset", 0x24u, 400u},
                              {24, "AGraphical", 0x10u, 200u}};
constexpr std::uint32_t kPoolAlign = 0x10u;
constexpr std::uint32_t kPoolFactor = 2u;  // pools are built for 200 %
constexpr std::uint32_t kReserve = 4u;     // NPC slots kept free for things the cap does not count (scripted NPCs, a burst of spawns)
constexpr std::uint32_t kHeapState = 0x08ADD25Cu;  // the game's dlmalloc state: +8 designated victim size, +0xC top chunk size
constexpr std::uint32_t kHeapFloor = 1024u * 1024u;  // below this much free heap (top + victim) the cap stops growing: every extra NPC costs about 75 KB

struct Cell {
    std::uint32_t base_cap = 0u, written_cap = 0u, base_list_max = 0u, written_list_max = 0u;
    float base_a = 0.0f, base_b = 0.0f;
    std::uint32_t written_a = 0u, written_b = 0u;
    // The setting this cell runs with: the one that stood when the game made the cell (a level load, a retry), so that a change of
    // perf.enemy_density in the menu applies at the next mission, as the menu says, and not half way through a fight.
    int percent = 100;
    std::uint32_t serial = 0;  // view::State::level_serial when the percent was taken
};

struct State {
    bool pools_done = false;
    bool pools_resized = false;
    std::uint64_t next_poll = 0;
    int percent = 100;
    std::unordered_map<std::uint32_t, Cell> cells;
    bool verbose = std::getenv("INFECTED_TUNE_LOG") != nullptr;
};

State &state() {
    static State s;
    return s;
}

std::uint32_t pool_object(Runtime &runtime, int id) {
    const std::uint32_t a = runtime.memory().load32(kRegistry + 4u * static_cast<std::uint32_t>(id));
    return view::in_ram(a) ? a : 0u;
}

// Rebuild every pool for `kPoolFactor` times its size. Done once, at the first flip where the pools exist and nothing is allocated.
// Returns false to try again later.
bool resize_pools(Runtime &runtime, std::uint64_t flip) {
    auto &mem = runtime.memory();
    std::uint32_t objects[sizeof(kPools) / sizeof(kPools[0])];
    for (std::size_t i = 0; i < sizeof(kPools) / sizeof(kPools[0]); ++i) {
        const std::uint32_t pool = pool_object(runtime, kPools[i].id);
        if (pool == 0u || mem.load32(pool + kPoolState) != 1u) return false;      // not built yet
        if (mem.load32(pool + kPoolUsed) != 0u) return false;                     // in use: leave alone
        if (mem.load32(pool + kPoolCount) != kPools[i].count) {                   // not the game this was written for
            std::fprintf(stderr, "[tune] pool %s has %u entries, expected %u: pools left as they are\n", kPools[i].name,
                         mem.load32(pool + kPoolCount), kPools[i].count);
            return true;
        }
        objects[i] = pool;
    }
    const psprecomp::AllegrexContext saved = g_cpu;
    bool ok = true;
    for (std::size_t i = 0; i < sizeof(kPools) / sizeof(kPools[0]) && ok; ++i) {
        const PoolDef &def = kPools[i];
        auto rebuild = [&](std::uint32_t count) {
            g_cpu.gpr[4] = objects[i];
            pk_guest_call(kPoolTerm);
            g_cpu.gpr[4] = objects[i];
            g_cpu.gpr[5] = def.element;
            g_cpu.gpr[6] = count;
            g_cpu.gpr[7] = kPoolAlign;
            pk_guest_call(kPoolInit);
            return mem.load32(objects[i] + kPoolStorage) != 0u && mem.load32(objects[i] + kPoolFree) != 0u &&
                   mem.load32(objects[i] + kPoolCount) == count;
        };
        if (!rebuild(def.count * kPoolFactor)) {
            std::fprintf(stderr, "[tune] pool %s: no room for %u entries, restoring %u\n", def.name, def.count * kPoolFactor, def.count);
            ok = rebuild(def.count);
            break;
        }
    }
    g_cpu = saved;
    std::fprintf(stderr, "[tune] flip %llu: enemy density pools %s\n", static_cast<unsigned long long>(flip), ok ? "enlarged x2" : "left at their size (heap full)");
    return true;
}

std::uint32_t list_size(psprecomp::GuestMemory &mem, std::uint32_t cell, std::uint32_t index) {
    const std::uint32_t list = mem.load32(cell + kCellLists + 4u * index);
    if (!view::in_ram(list)) return 0u;
    const std::uint32_t inner = mem.load32(list + 8u);
    if (!view::in_ram(inner)) return 0u;
    return mem.load32(inner + 0x14u);
}

void frame(Runtime &runtime, std::uint64_t flip) {
    State &s = state();
    auto &mem = runtime.memory();
    if (flip < s.next_poll) return;
    s.next_poll = flip + 4u;
    const int percent = std::clamp(tuning_setting(portablekit::port::Key::PerfEnemyDensity, 100), 100, 200);
    // The pools are rebuilt the first time the setting is above 100 while every pool is unused (start-up, or the front-end after the setting was changed in a menu);
    // resize_pools refuses while anything is allocated and is asked again four flips later. At 100 nothing is touched.
    if (!s.pools_done && percent > 100 && resize_pools(runtime, flip)) { s.pools_done = true; s.pools_resized = true; }
    if (percent != s.percent) {
        std::fprintf(stderr, "[tune] flip %llu: enemy density setting %d%% -> %d%% (%s)\n", static_cast<unsigned long long>(flip), s.percent, percent,
                     s.cells.empty() ? "applies to the next mission" : "applies at the next mission, the cells of this one keep their number");
        s.percent = percent;
    }

    const std::uint32_t manager = mem.load32(kCellManager);
    if (!view::in_ram(manager)) { s.cells.clear(); return; }
    const std::uint32_t begin = mem.load32(manager + 0x28u), end = mem.load32(manager + 0x2Cu);
    if (!view::in_ram(begin) || end < begin || end - begin > 4u * 32u) { s.cells.clear(); return; }

    // free room of the pools that bound the number of NPCs
    std::uint32_t room = 0xFFFFu;
    for (int id : {1, 2}) {
        const std::uint32_t pool = pool_object(runtime, id);
        if (pool == 0u) continue;
        const std::uint32_t count = mem.load32(pool + kPoolCount), used = mem.load32(pool + kPoolUsed);
        const std::uint32_t free_slots = count > used ? count - used : 0u;
        room = std::min(room, free_slots > kReserve ? free_slots - kReserve : 0u);
    }

    // and the heap: with less than kHeapFloor free the cap is not raised any further (it never goes below what the level asks for)
    const std::uint32_t heap_free = mem.load32(kHeapState + 8u) + mem.load32(kHeapState + 0xCu);
    if (heap_free < kHeapFloor) room = 0u;

    std::unordered_map<std::uint32_t, bool> seen;
    for (std::uint32_t p = begin; p < end; p += 4u) {
        const std::uint32_t cell = mem.load32(p);
        if (!view::in_ram(cell) || mem.load32(cell + 4u) != kCellVtable) continue;
        seen[cell] = true;
        Cell &c = s.cells[cell];
        const std::uint32_t cap_now = mem.load32(cell + kCellMax);
        const bool fresh = c.written_cap == 0u;
        const bool relearn = fresh || cap_now != c.written_cap;
        if (relearn) {  // first sight, or the game wrote the cell again
            c.base_cap = cap_now;
            c.base_a = as_float(mem.load32(cell + kCellGapA));
            c.base_b = as_float(mem.load32(cell + kCellGapB));
            c.written_cap = cap_now;
            c.base_list_max = c.written_list_max = mem.load32(cell + kCellListMax);
            c.written_a = mem.load32(cell + kCellGapA);
            c.written_b = mem.load32(cell + kCellGapB);
            if (s.verbose) std::fprintf(stderr, "[tune] flip %llu cell 0x%08x cap %u gap %g..%g\n", static_cast<unsigned long long>(flip), cell, c.base_cap, c.base_a, c.base_b);
        }
        if (relearn || c.serial != view::state().level_serial) {  // a new cell, or the same object in a new level (a retry): the setting of this moment
            c.serial = view::state().level_serial;
            c.percent = percent;
        }
        if (c.base_cap == 0u || c.base_cap > 64u) continue;  // not a spawning cell
        const std::uint32_t cell_percent = static_cast<std::uint32_t>(c.percent);   // this cell's, not the live setting
        const std::uint32_t scaled = std::max(c.base_cap, (c.base_cap * cell_percent + 50u) / 100u);
        const std::uint32_t alive = list_size(mem, cell, 1u) + list_size(mem, cell, 2u);
        const std::uint32_t target = std::max(c.base_cap, std::min(scaled, alive + room));
        if (target != c.written_cap) {
            mem.store32(cell + kCellMax, target);
            c.written_cap = target;
        }
        // the cell also limits how many NPCs of its second list (the ones the level counts as the horde) may exist: scale that limit alike
        if (c.base_list_max != 0u && c.base_list_max < 1000u) {
            const std::uint32_t list_max = std::max(c.base_list_max, (c.base_list_max * cell_percent + 50u) / 100u);
            if (list_max != c.written_list_max) {
                mem.store32(cell + kCellListMax, list_max);
                c.written_list_max = list_max;
            }
        }
        const float k = 100.0f / static_cast<float>(cell_percent);
        const std::uint32_t wa = view::bits_of(c.base_a * k), wb = view::bits_of(c.base_b * k);
        if (wa != c.written_a || wb != c.written_b) {
            mem.store32(cell + kCellGapA, wa);
            mem.store32(cell + kCellGapB, wb);
            c.written_a = wa;
            c.written_b = wb;
        }
    }
    for (auto it = s.cells.begin(); it != s.cells.end();) it = seen.count(it->first) ? std::next(it) : s.cells.erase(it);
}

// developer probe: INFECTED_TUNE_CELLS=N prints every N flips, per cell, id, cap, NPCs alive (two lists), state, overload timer and the remaining
// counts of its generators
void cell_probe(Runtime &runtime, std::uint64_t flip) {
    static const long every = [] {
        const char *t = std::getenv("INFECTED_TUNE_CELLS");
        return t != nullptr ? std::strtol(t, nullptr, 10) : 0l;
    }();
    if (every <= 0 || flip % static_cast<std::uint64_t>(every) != 0u) return;
    auto &mem = runtime.memory();
    const std::uint32_t manager = mem.load32(kCellManager);
    if (!view::in_ram(manager)) return;
    const std::uint32_t begin = mem.load32(manager + 0x28u), end = mem.load32(manager + 0x2Cu);
    if (!view::in_ram(begin) || end < begin || end - begin > 4u * 32u) return;
    std::fprintf(stderr, "[cells] flip %llu:", static_cast<unsigned long long>(flip));
    for (std::uint32_t p = begin; p < end; p += 4u) {
        const std::uint32_t cell = mem.load32(p);
        if (!view::in_ram(cell) || mem.load32(cell + 4u) != kCellVtable) continue;
        std::fprintf(stderr, " [id%u cap=%u alive=%u+%u st=%u ovl=%.0f gens", mem.load32(cell + 0x94u), mem.load32(cell + kCellMax), list_size(mem, cell, 1u),
                     list_size(mem, cell, 2u), mem.load32(cell + 0x230u), as_float(mem.load32(cell + 0x180u)));
        const std::uint32_t n = std::min<std::uint32_t>(mem.load32(cell + 0x64u), 16u);
        for (std::uint32_t i = 0; i < n; ++i) {
            const std::uint32_t g = mem.load32(cell + 0x3Cu + 4u * i);
            if (view::in_ram(g)) std::fprintf(stderr, " %d", static_cast<int>(mem.load32(g + 0x11Cu)));
        }
        std::fprintf(stderr, "]");
    }
    std::fprintf(stderr, "\n");
}

} // namespace density

// The live entries of a fixed-size pool: alloc hands out free[used++] and a free pushes the pointer back at free[--used], so the entries
// free[used .. count) are the free ones and everything else in the storage is in use.
std::vector<std::uint32_t> live_objects(psprecomp::GuestMemory &mem, std::uint32_t pool) {
    std::vector<std::uint32_t> live;
    const std::uint32_t elem = mem.load32(pool + 0x64u), count = mem.load32(pool + 0x68u), used = mem.load32(pool + 0x6Cu);
    const std::uint32_t storage = mem.load32(pool + 0x78u), list = mem.load32(pool + 0x7Cu);
    if (!view::in_ram(storage) || !view::in_ram(list) || count > 4096u || used > count || elem == 0u) return live;
    std::vector<std::uint32_t> freed;
    for (std::uint32_t i = used; i < count; ++i) freed.push_back(mem.load32(list + 4u * i));
    std::sort(freed.begin(), freed.end());
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t p = storage + i * elem;
        if (!std::binary_search(freed.begin(), freed.end(), p)) live.push_back(p);
    }
    return live;
}

// developer harness for the completion tests, off by default:
//   INFECTED_TUNE_GOD=1      the player never loses health (his current health +0x31C is set to his maximum +0x318 every flip; the player is the target
//                            +0x358 of the live NPCs, vtable 0x08ACE3CC at +0x14)
//   INFECTED_TUNE_SUPPLY=k   every generator of every cell starts with at most k enemies left (a short level for a scripted player that cannot kill 37)
void harness(Runtime &runtime, std::uint64_t flip) {
    static const bool god = std::getenv("INFECTED_TUNE_GOD") != nullptr;
    static const long supply = [] {
        const char *t = std::getenv("INFECTED_TUNE_SUPPLY");
        return t != nullptr ? std::strtol(t, nullptr, 10) : -1l;
    }();
    if (!god && supply < 0) return;
    auto &mem = runtime.memory();
    static std::uint32_t player = 0;
    static std::unordered_map<std::uint32_t, bool> clipped;
    if (god) {
        const std::uint32_t pool = mem.load32(0x09EAC5E4u + 4u);
        if (view::in_ram(pool))
            for (const std::uint32_t npc : live_objects(mem, pool)) {
                const std::uint32_t target = mem.load32(npc + 0x358u);
                if (view::in_ram(target) && mem.load32(target + 0x14u) == 0x08ACE3CCu) player = target;
            }
        if (view::in_ram(player) && mem.load32(player + 0x14u) == 0x08ACE3CCu) mem.store32(player + 0x31Cu, mem.load32(player + 0x318u));
    }
    static std::uint32_t last_world = 0;
    const std::uint32_t world = mem.load32(view::kWorldSingleton);
    if (world != last_world) {  // a new level (a retry builds one): clip its generators again
        last_world = world;
        clipped.clear();
    }
    if (supply >= 0 && flip % 8u == 0u) {
        const std::uint32_t manager = mem.load32(density::kCellManager);
        if (!view::in_ram(manager)) return;
        const std::uint32_t begin = mem.load32(manager + 0x28u), end = mem.load32(manager + 0x2Cu);
        if (!view::in_ram(begin) || end < begin || end - begin > 4u * 32u) return;
        for (std::uint32_t p = begin; p < end; p += 4u) {
            const std::uint32_t cell = mem.load32(p);
            if (!view::in_ram(cell) || mem.load32(cell + 4u) != density::kCellVtable) continue;
            const std::uint32_t n = std::min<std::uint32_t>(mem.load32(cell + 0x64u), 16u);
            for (std::uint32_t i = 0; i < n; ++i) {
                const std::uint32_t g = mem.load32(cell + 0x3Cu + 4u * i);
                if (!view::in_ram(g) || clipped[g]) continue;
                clipped[g] = true;
                if (static_cast<long>(mem.load32(g + 0x11Cu)) > supply) mem.store32(g + 0x11Cu, static_cast<std::uint32_t>(supply));
            }
        }
    }
}

// developer stress harness: INFECTED_TUNE_KILL=from:age[:perround] stands in for a player who kills. From flip `from`, every NPC of the Npc pool
// that has been alive for `age` flips is hit with a lethal blow through the game's own Npc::TakeDamage (0x0881F6FC; health is +0x31C, state
// bits +0x59), at most `perround` per flip, so the game's own death, corpse, drop, counting and refill paths run. Never on by default.
std::uint64_t g_flip_now = 0;  // the flip being run, for code that runs inside the game thread
const bool g_kill_in_thread = std::getenv("INFECTED_TUNE_KILL_HOOK") != nullptr;  // the blows are struck from the cell tick, not from the flip hook

void kill_pass(Runtime &runtime, std::uint64_t flip) {
    static const std::string spec = [] {
        const char *t = std::getenv("INFECTED_TUNE_KILL");
        return std::string(t != nullptr ? t : "");
    }();
    if (spec.empty()) return;
    unsigned long long from = 0, age = 0, per = 1;
    if (std::sscanf(spec.c_str(), "%llu:%llu:%llu", &from, &age, &per) < 2 || flip < from) return;
    static std::unordered_map<std::uint32_t, std::uint64_t> born;
    static std::unordered_map<std::uint32_t, std::uint64_t> hit;  // flip of the lethal blow per victim
    auto &mem = runtime.memory();
    const std::uint32_t pool = mem.load32(0x09EAC5E4u + 4u);
    if (!view::in_ram(pool)) return;
    std::unordered_map<std::uint32_t, std::uint64_t> now;
    unsigned long long killed = 0;
    for (const std::uint32_t npc : live_objects(mem, pool)) {
        auto it = born.find(npc);
        const std::uint64_t t0 = it == born.end() ? flip : it->second;
        now[npc] = t0;
        if (flip - t0 >= age && killed < per && as_float(mem.load32(npc + 0x31Cu)) > 0.0f && (mem.load8(npc + 0x59u) & 3u) == 0u) {
            // the game's own hit path: 0x088A54F0(.., victim, attacker, 1) with the damage in $f12 checks the teams, calls Npc::TakeDamage (vtable +0xF8,
            // 0x0881F6FC) and reports the damage; the attacker is the NPC's current target (the player)
            const std::uint32_t attacker = mem.load32(npc + 0x358u);
            if (!view::in_ram(attacker)) continue;
            static const float dmg = [] {
                const char *t = std::getenv("INFECTED_TUNE_KILL_DMG");
                return t != nullptr ? static_cast<float>(std::strtod(t, nullptr)) : 100000.0f;
            }();
            const unsigned before = mem.load8(npc + 0x58u) | (mem.load8(npc + 0x59u) << 8);
            const psprecomp::AllegrexContext saved = g_cpu;
            g_cpu.gpr[4] = mem.load32(0x08AA8740u);
            g_cpu.gpr[5] = npc;
            g_cpu.gpr[6] = attacker;
            g_cpu.gpr[7] = 1u;
            g_cpu.fpr[12] = dmg > 0.0f ? dmg : as_float(mem.load32(npc + 0x31Cu));  // 0: exactly the victim's health
            pk_guest_call(0x088A54F0u);
            g_cpu = saved;
            hit[npc] = flip;
            static unsigned long long blows = 0;
            static const long watch_nth = [] {
                const char *t = std::getenv("INFECTED_TUNE_KILLWATCH");  // watch the writes to the Nth victim's state bytes (PSPRECOMP_WATCH_WRITE=1 arms it)
                return t != nullptr ? std::strtol(t, nullptr, 10) : 0l;
            }();
            if (static_cast<long>(++blows) == watch_nth) psprecomp::set_write_watch(npc + 0x58u, 4u);
            std::fprintf(stderr, "[kill] flip %llu npc %08x age %llu state58/59 %04x -> %04x hp %g at (%.0f %.0f %.0f) player (%.0f %.0f %.0f)\n",
                         static_cast<unsigned long long>(flip), npc, static_cast<unsigned long long>(flip - t0), before,
                         mem.load8(npc + 0x58u) | (mem.load8(npc + 0x59u) << 8), as_float(mem.load32(npc + 0x31Cu)), as_float(mem.load32(npc + 0x4F0u)),
                         as_float(mem.load32(npc + 0x4F4u)), as_float(mem.load32(npc + 0x4F8u)), as_float(mem.load32(attacker + 0x4F0u)),
                         as_float(mem.load32(attacker + 0x4F4u)), as_float(mem.load32(attacker + 0x4F8u)));
            std::fprintf(stderr, "[kill]   words +1ac %08x +2d4 %08x +2fc %08x +3a8 %08x +408 %08x +4a0 %08x +4b0 %08x +4b4 %08x +510 %08x\n", mem.load32(npc + 0x1ACu), mem.load32(npc + 0x2D4u),
                         mem.load32(npc + 0x2FCu), mem.load32(npc + 0x3A8u), mem.load32(npc + 0x408u), mem.load32(npc + 0x4A0u), mem.load32(npc + 0x4B0u), mem.load32(npc + 0x4B4u),
                         mem.load32(npc + 0x510u));
            {  // the world camera's position (its frame's LTM, frame + 0x80) and its distance to the victim
                const std::uint32_t cam = mem.load32(0x09ED32C0u), frame = view::in_ram(cam) ? mem.load32(cam + 4u) : 0u;
                if (view::in_ram(frame)) {
                    const float cx = as_float(mem.load32(frame + 0x80u)), cy = as_float(mem.load32(frame + 0x84u)), cz = as_float(mem.load32(frame + 0x88u));
                    const float dx = cx - as_float(mem.load32(npc + 0x4F0u)), dy = cy - as_float(mem.load32(npc + 0x4F4u)), dz = cz - as_float(mem.load32(npc + 0x4F8u));
                    std::fprintf(stderr, "[kill]   camera (%.0f %.0f %.0f) at (%.2f %.2f %.2f) distance %.0f\n", cx, cy, cz, as_float(mem.load32(frame + 0x70u)),
                                 as_float(mem.load32(frame + 0x74u)), as_float(mem.load32(frame + 0x78u)), std::sqrt(dx * dx + dy * dy + dz * dz));
                }
            }
            ++killed;
        }
    }
    for (const auto &entry : born)
        if (!now.count(entry.first) && hit.count(entry.first)) {
            std::fprintf(stderr, "[kill] flip %llu npc %08x removed %llu flips after the blow\n", static_cast<unsigned long long>(flip), entry.first,
                         static_cast<unsigned long long>(flip - hit[entry.first]));
            hit.erase(entry.first);
        }
    born.swap(now);
}

void kill_harness(Runtime &runtime, std::uint64_t flip) {
    if (!g_kill_in_thread) kill_pass(runtime, flip);
}

// INFECTED_TUNE_KILL_HOOK=1: the same blows, struck from inside the game's own thread. The cell tick (0x088BD9DC, vtable 0x08AD2570 + 0x14, always called
// through the vtable) is replaced by a wrapper that makes the pass once per flip, then runs the original lean code.
constexpr std::uint32_t kCellTick = 0x088BD9DCu;
void cell_tick_wrapper(psprecomp::CorpusRuntime &corpus, psprecomp::AllegrexContext &ctx) {
    static const psprecomp::LeanEntry original = psprecomp::exec_lean_entry(kCellTick);
    static std::uint64_t last_pass = ~0ull;
    if (g_flip_now != last_pass) {
        last_pass = g_flip_now;
        const psprecomp::AllegrexContext saved = ctx;
        kill_pass(static_cast<Runtime &>(corpus), g_flip_now);
        ctx = saved;
    }
    if (original != nullptr) original();
    ctx.pc = ctx.gpr[31];
}

void install_kill_hook(Runtime &runtime) {
    if (!g_kill_in_thread || std::getenv("INFECTED_TUNE_KILL") == nullptr) return;
    runtime.register_function(kCellTick, &cell_tick_wrapper, "infected_cell_tick");
    std::fprintf(stderr, "[tune] kill harness: cell tick 0x%08x wrapped\n", kCellTick);
}

// developer probe: INFECTED_TUNE_POOLS=N prints the used / peak / capacity of the game's fixed-size pools every N flips
void pool_probe(Runtime &runtime, std::uint64_t flip) {
    static const long every = [] {
        const char *t = std::getenv("INFECTED_TUNE_POOLS");
        return t != nullptr ? std::strtol(t, nullptr, 10) : 0l;
    }();
    if (every <= 0 || flip % static_cast<std::uint64_t>(every) != 0u) return;
    auto &mem = runtime.memory();
    constexpr std::uint32_t kRegistry = 0x09EAC5E4u;  // allocator table: pointer per id
    static const struct { int id; const char *name; } kPools[] = {{0, "Stl"}, {1, "Npc"}, {2, "NpcGhost"}, {3, "Pickup"}, {4, "Projectile"}, {5, "Weapon"},
                                                                  {6, "FxSimple"}, {7, "ACollidable"}, {8, "VoiceHandle"}, {17, "GraphicalAsset"}, {24, "AGraphical"}};
    std::fprintf(stderr, "[pool] flip %llu", static_cast<unsigned long long>(flip));
    for (const auto &p : kPools) {
        const std::uint32_t a = mem.load32(kRegistry + 4u * static_cast<std::uint32_t>(p.id));
        if (a < 0x08800000u || a >= 0x0A000000u - 0x100u) continue;
        std::fprintf(stderr, " %s=%u/%u(pk %u)", p.name, mem.load32(a + 0x6c), mem.load32(a + 0x68), mem.load32(a + 0x70));
    }
    std::fprintf(stderr, "\n");
}

// developer probe: INFECTED_TUNE_GENS=N lists the CNpcGen objects (vtable 0x08ACE33C at +0x14) every N flips with their remaining counts;
// INFECTED_TUNE_WATCHGEN=flip:index then points the write watch (PSPRECOMP_WATCH_WRITE=1 arms it) at that generator's count (+0x11c).
void gen_probe(Runtime &runtime, std::uint64_t flip) {
    static const long every = [] {
        const char *t = std::getenv("INFECTED_TUNE_GENS");
        return t != nullptr ? std::strtol(t, nullptr, 10) : 0l;
    }();
    static const std::string watch = [] {
        const char *t = std::getenv("INFECTED_TUNE_WATCHGEN");
        return std::string(t != nullptr ? t : "");
    }();
    unsigned long long wflip = 0, windex = 0;
    const bool want_watch = !watch.empty() && std::sscanf(watch.c_str(), "%llu:%llu", &wflip, &windex) == 2 && wflip == flip;
    if (!want_watch && (every <= 0 || flip % static_cast<std::uint64_t>(every) != 0u)) return;
    auto &mem = runtime.memory();
    std::uint32_t found[16];
    int n = 0;
    for (std::uint32_t a = 0x08800000u; a < 0x0A000000u - 0x200u && n < 16; a += 4u)
        if (mem.load32(a + 0x14u) == 0x08ACE33Cu && mem.load32(a + 0xC4u) == 0x08ACE384u) found[n++] = a;
    std::fprintf(stderr, "[gens] flip %llu:", static_cast<unsigned long long>(flip));
    for (int i = 0; i < n; ++i) std::fprintf(stderr, " %08x(rem=%d)", found[i], static_cast<int>(mem.load32(found[i] + 0x11Cu)));
    std::fprintf(stderr, "\n");
    if (want_watch && windex < static_cast<unsigned long long>(n)) psprecomp::set_write_watch(found[windex] + 0x11Cu, 4u);
}

// developer probe: INFECTED_TUNE_NPCS=flip[:count] prints the live NPC objects of the Npc pool and the first `count` (default 1) in full
void npc_probe(Runtime &runtime, std::uint64_t flip) {
    static const std::string spec = [] {
        const char *t = std::getenv("INFECTED_TUNE_NPCS");
        return std::string(t != nullptr ? t : "");
    }();
    unsigned long long f = 0, count = 1;
    if (spec.empty() || std::sscanf(spec.c_str(), "%llu:%llu", &f, &count) < 1 || f != flip) return;
    auto &mem = runtime.memory();
    const std::uint32_t pool = mem.load32(0x09EAC5E4u + 4u);
    if (!view::in_ram(pool)) return;
    const std::vector<std::uint32_t> live = live_objects(mem, pool);
    std::fprintf(stderr, "[npcs] flip %llu used=%u live=%u storage=%08x:", f, mem.load32(pool + 0x6c), static_cast<unsigned>(live.size()), mem.load32(pool + 0x78));
    for (std::uint32_t npc : live) std::fprintf(stderr, " %08x(st%02x,hp%g)", npc, mem.load8(npc + 0x59u), as_float(mem.load32(npc + 0x31Cu)));
    std::fprintf(stderr, "\n");
    for (std::size_t i = 0; i < live.size() && i < count; ++i) {
        const std::uint32_t npc = live[i];
        for (std::uint32_t w = 0; w < 0x540u / 4u; ++w) {
            if (w % 8 == 0) std::fprintf(stderr, "%s[npc%u] %08x:", w ? "\n" : "", static_cast<unsigned>(i), npc + w * 4u);
            std::fprintf(stderr, " %08x", mem.load32(npc + w * 4u));
        }
        std::fprintf(stderr, "\n");
    }
}

// developer probe: INFECTED_TUNE_DUMP="flip:addr:words,flip:addr:words" prints guest memory at those flips (hex addr)
void dump_probe(Runtime &runtime, std::uint64_t flip) {
    static const std::string spec = [] {
        const char *t = std::getenv("INFECTED_TUNE_DUMP");
        return std::string(t != nullptr ? t : "");
    }();
    if (spec.empty()) return;
    std::size_t pos = 0;
    while (pos < spec.size()) {
        std::size_t end = spec.find(',', pos);
        if (end == std::string::npos) end = spec.size();
        unsigned long long f = 0, a = 0, n = 0;
        if (std::sscanf(spec.substr(pos, end - pos).c_str(), "%llu:%llx:%llu", &f, &a, &n) == 3 && f == flip) {
            auto &mem = runtime.memory();
            for (unsigned long long i = 0; i < n; ++i) {
                if (i % 8 == 0) std::fprintf(stderr, "%s[dump %llu] %08llx:", i ? "\n" : "", f, a + i * 4);
                std::fprintf(stderr, " %08x", mem.load32(static_cast<std::uint32_t>(a + i * 4)));
            }
            std::fprintf(stderr, "\n");
        }
        pos = end + 1;
    }
}

namespace {
void (*g_flip_observer)(std::uint64_t) = nullptr;
}
void set_flip_observer(void (*observer)(std::uint64_t flip)) { g_flip_observer = observer; }

void tuning_frame(Runtime &runtime, std::uint64_t flip) {
    g_flip_now = flip;
    if (g_flip_observer != nullptr) g_flip_observer(flip);
    static bool said = false;
    if (!said) {
        said = true;
        std::fprintf(stderr, "[tune] flip %llu: perf.enemy_density=%d%% perf.view_distance=%d%% (changes show in this log as '[tune] ... enemy density' lines when INFECTED_TUNE_LOG is set)\n",
                     static_cast<unsigned long long>(flip), tuning_setting(portablekit::port::Key::PerfEnemyDensity, 100),
                     tuning_setting(portablekit::port::Key::PerfViewDistance, 100));
    }
    bot::frame(runtime, flip);
    dump_probe(runtime, flip);
    npc_probe(runtime, flip);
    pool_probe(runtime, flip);
    gen_probe(runtime, flip);
    view::frame(runtime, flip);
    density::frame(runtime, flip);
    kill_harness(runtime, flip);
    harness(runtime, flip);
    density::cell_probe(runtime, flip);
    // developer probe: INFECTED_TUNE_CAMLOG=N prints the current RwCamera's near/far/fog planes every N flips
    static const long every = [] {
        const char *t = std::getenv("INFECTED_TUNE_CAMLOG");
        return t != nullptr ? std::strtol(t, nullptr, 10) : 0l;
    }();
    if (every > 0 && flip % static_cast<std::uint64_t>(every) == 0u) {
        auto &mem = runtime.memory();
        const std::uint32_t cam = mem.load32(0x09ED32C0u);
        if (cam >= 0x08800000u && cam < 0x0A000000u)
            std::fprintf(stderr, "[tune] head: %08x %08x %08x %08x %08x %08x\n", mem.load32(cam), mem.load32(cam + 4), mem.load32(cam + 8), mem.load32(cam + 12), mem.load32(cam + 16), mem.load32(cam + 20));
        if (cam >= 0x08800000u && cam < 0x0A000000u)
            std::fprintf(stderr, "[tune] flip %llu cam=0x%08x near=%g far=%g fog=%g zscale=%g zshift=%g\n", static_cast<unsigned long long>(flip), cam,
                         as_float(mem.load32(cam + 0x80)), as_float(mem.load32(cam + 0x84)), as_float(mem.load32(cam + 0x88)),
                         as_float(mem.load32(cam + 0x8c)), as_float(mem.load32(cam + 0x90)));
        else
            std::fprintf(stderr, "[tune] flip %llu cam=0x%08x (not a pointer)\n", static_cast<unsigned long long>(flip), cam);
    }
}

} // namespace infected
