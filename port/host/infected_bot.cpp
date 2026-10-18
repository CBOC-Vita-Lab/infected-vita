#include "infected_bot.hpp"

#include "input/pad_provider.hpp"
#include "kernel/kernel.hpp"
#include "psprecomp/corpus_abi.hpp"
#include "psprecomp/exec.hpp"
#include "psprecomp/runtime.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace infected::bot {
namespace {

using portablekit::PadSample;
constexpr std::uint32_t kRamLo = 0x08800000u, kRamHi = 0x0A000000u;

inline bool in_ram(std::uint32_t a) { return a >= kRamLo && a < kRamHi - 0x400u; }
inline std::uint32_t rd32(std::uint32_t a) { std::uint32_t v; std::memcpy(&v, PK_GUEST_TO_HOST(a), 4); return v; }
inline std::uint8_t rd8(std::uint32_t a) { return *PK_GUEST_TO_HOST(a); }
inline float rdf(std::uint32_t a) { float f; std::memcpy(&f, PK_GUEST_TO_HOST(a), 4); return f; }

constexpr std::uint32_t kPlayerVtable = 0x08ACE3CCu;  // CInfectedPlayer, word at +0x14
constexpr std::uint32_t kPausedMode = 0x08AACAFCu;     // engine mode word: 1 running, 2 paused by a tutorial dialog
constexpr std::uint32_t kPoolRegistry = 0x09EAC5E4u;  // allocator table, pointer per id; id 1 = Npc

std::uint32_t find_player() {
    // the player object is the only one with this vtable word at +0x14
    const auto *words = reinterpret_cast<const std::uint32_t *>(PK_GUEST_TO_HOST(kRamLo));
    for (std::uint32_t a = kRamLo; a < kRamHi - 0x600u; a += 4u)
        if (words[((a - kRamLo) >> 2) + 5u] == kPlayerVtable) return a;
    return 0u;
}

std::vector<std::uint32_t> live_npcs() {
    std::vector<std::uint32_t> live;
    const std::uint32_t pool = rd32(kPoolRegistry + 4u);
    if (!in_ram(pool)) return live;
    const std::uint32_t elem = rd32(pool + 0x64u), count = rd32(pool + 0x68u), used = rd32(pool + 0x6Cu);
    const std::uint32_t storage = rd32(pool + 0x78u), list = rd32(pool + 0x7Cu);
    if (!in_ram(storage) || !in_ram(list) || count > 4096u || used > count || elem == 0u) return live;
    std::vector<std::uint32_t> freed;
    for (std::uint32_t i = used; i < count; ++i) freed.push_back(rd32(list + 4u * i));
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t p = storage + i * elem;
        bool is_free = false;
        for (std::uint32_t f : freed) if (f == p) { is_free = true; break; }
        if (!is_free) live.push_back(p);
    }
    return live;
}

// INFECTED_BOT_SNAP=flip,flip,...: write the 24 MB of guest RAM (0x08800000..0x0A000000) to $INFECTED_BOT_SNAP_DIR/snap_<flip>.bin
void snap(std::uint64_t flip) {
    static const std::string spec = [] { const char *t = std::getenv("INFECTED_BOT_SNAP"); return std::string(t ? t : ""); }();
    if (spec.empty()) return;
    bool hit = false;
    std::size_t pos = 0;
    while (pos < spec.size()) {
        std::size_t end = spec.find(',', pos);
        if (end == std::string::npos) end = spec.size();
        if (std::strtoull(spec.c_str() + pos, nullptr, 10) == flip) hit = true;
        pos = end + 1;
    }
    if (!hit) return;
    const char *dir = std::getenv("INFECTED_BOT_SNAP_DIR");
    const std::string path = std::string(dir ? dir : ".") + "/snap_" + std::to_string(flip) + ".bin";
    if (std::FILE *f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(PK_GUEST_TO_HOST(kRamLo), 1, kRamHi - kRamLo, f);
        std::fclose(f);
        std::fprintf(stderr, "[bot] snapshot %s\n", path.c_str());
    }
}

void dump_object(const char *tag, std::uint32_t base, std::uint32_t bytes) {
    for (std::uint32_t o = 0; o < bytes; o += 32u) {
        std::fprintf(stderr, "[%s %08x] +%04x:", tag, base, o);
        for (std::uint32_t w = 0; w < 8u && o + w * 4u < bytes; ++w) std::fprintf(stderr, " %08x", rd32(base + o + w * 4u));
        std::fprintf(stderr, "\n");
    }
}

void probe(std::uint64_t flip) {
    static const std::string spec = [] { const char *t = std::getenv("INFECTED_BOT_PROBE"); return std::string(t ? t : ""); }();
    if (spec.empty()) return;
    bool hit = false;
    std::size_t pos = 0;
    while (pos < spec.size()) {
        std::size_t end = spec.find(',', pos);
        if (end == std::string::npos) end = spec.size();
        if (std::strtoull(spec.c_str() + pos, nullptr, 10) == flip) hit = true;
        pos = end + 1;
    }
    if (!hit) return;
    const std::uint32_t player = find_player();
    std::fprintf(stderr, "[bot-probe] flip %llu player 0x%08x\n", static_cast<unsigned long long>(flip), player);
    if (player != 0u) dump_object("player", player, 0x600u);
    if (player != 0u && std::getenv("INFECTED_BOT_PROBE_PTRS") != nullptr) {  // one level of pointers out of the player object
        std::vector<std::uint32_t> seen;
        for (std::uint32_t o = 0; o < 0x600u; o += 4u) {
            const std::uint32_t w = rd32(player + o);
            if (!in_ram(w) || (w & 3u) != 0u || (w >= player && w < player + 0x600u)) continue;
            bool dup = false;
            for (std::uint32_t q : seen) if (q == w) { dup = true; break; }
            if (dup) continue;
            seen.push_back(w);
            dump_object("ptr", w, 0x120u);
        }
    }
    for (std::uint32_t npc : live_npcs()) dump_object("npc", npc, 0x540u);
}

// INFECTED_BOT_TABLE=N: a compact table of the player and the live NPCs every N flips
void table(std::uint64_t flip) {
    static const long every = [] { const char *t = std::getenv("INFECTED_BOT_TABLE"); return t ? std::strtol(t, nullptr, 10) : 0l; }();
    if (every <= 0 || flip % static_cast<std::uint64_t>(every) != 0u) return;
    static std::uint32_t player = 0;
    if (!in_ram(player) || rd32(player + 0x14u) != kPlayerVtable) player = find_player();
    if (player != 0u) {
        const std::uint32_t body = rd32(player + 0x40Cu);
        std::fprintf(stderr, "[bot] flip %llu player %08x hp %g/%g pos (%.1f %.1f %.1f) h (%.3f %.3f) st %04x mode %u\n", static_cast<unsigned long long>(flip), player,
                     rdf(player + 0x31Cu), rdf(player + 0x318u), rdf(body + 0x30u), rdf(body + 0x34u), rdf(body + 0x38u), rdf(body + 0x48u), rdf(body + 0x50u),
                     rd8(player + 0x58u) | (rd8(player + 0x59u) << 8), rd32(kPausedMode));
    } else {
        return;
    }
    for (std::uint32_t npc : live_npcs())
    {
        const std::uint32_t nb = rd32(npc + 0x40Cu);
        std::fprintf(stderr, "[bot]   npc %08x vt %08x hp %g/%g thr %g pos (%.1f %.1f %.1f) st %04x w328 %g body (%.1f %.1f %.1f)\n", npc, rd32(npc + 0x14u), rdf(npc + 0x31Cu), rdf(npc + 0x318u),
                     rdf(npc + 0x320u), rdf(npc + 0x4F0u), rdf(npc + 0x4F4u), rdf(npc + 0x4F8u), rd8(npc + 0x58u) | (rd8(npc + 0x59u) << 8), rdf(npc + 0x328u),
                     in_ram(nb) ? rdf(nb + 0x30u) : 0.0f, in_ram(nb) ? rdf(nb + 0x34u) : 0.0f, in_ram(nb) ? rdf(nb + 0x38u) : 0.0f);
        const std::uint32_t nf = rd32(npc + 0x3Cu);
        std::fprintf(stderr, "[bot]     frame %08x pos (%.1f %.1f %.1f)\n", nf, in_ram(nf) ? rdf(nf + 0x40u) : 0.0f, in_ram(nf) ? rdf(nf + 0x44u) : 0.0f, in_ram(nf) ? rdf(nf + 0x48u) : 0.0f);
    }
}

// ---- the bot ------------------------------------------------------------------------------------------------------------------------
// PSP pad bits
constexpr std::uint32_t kUp = 0x0010u, kRight = 0x0020u, kDown = 0x0040u, kLeft = 0x0080u;
constexpr std::uint32_t kSelect = 0x0001u, kStart = 0x0008u, kL = 0x0100u, kR = 0x0200u, kTriangle = 0x1000u, kCircle = 0x2000u, kCross = 0x4000u, kSquare = 0x8000u;

struct Params {
    float approach = 140.0f;   // keep walking towards the target while it is farther than this
    float fire_range = 170.0f; // pistol range used by the bot
    float aim_cone = 10.0f;    // degrees
    int lock = 1;              // hold R (lock-on) while a target is near
    std::uint32_t prim = 0x4000u, sec = 0x8000u;  // buttons: weaken with `prim` (CROSS, pistol), finish with `sec` (SQUARE, viral gun)
    int sec_after = 0;         // do not use `sec` before this flip (viral charge experiments)
    int pulse = 3;             // fire button: pressed `pulse` flips, released `pulse` flips (0 = held)
    int pulse2 = 3;            // same for the finishing button
    int lock2 = 1;             // hold R while finishing
    float back = 0.0f;         // back away (stick down) while the target is closer than this
    int fresh = 0;             // blank memory stick: after the officer name entry has been typed (CROSS 10 times) move to SAVE (RIGHT x8, DOWN x6) and accept
    int phase = 0;             // shifts the menu-press timing (flips), to get differently-timed but equivalent runs
    int retry = 0;             // results screen: 1 = pick RETRY, 0 = pick CAMPAIGN
    int verbose = 0;
};

struct Bot {
    bool enabled = false;
    Params par;
    std::uint32_t player = 0;
    std::uint64_t flip = 0;
    std::uint64_t world_since = 0;     // flip since which the world singleton has been present (0 = absent)
    std::uint64_t playing_since = 0;
    std::uint64_t modal_since = 0;     // flip at which the engine mode word left 1 (dialog, briefing, results)
    PadSample out;                     // what the pad provider returns
    std::uint64_t last_log = 0;
    unsigned alive = 0;                // threats counted in the last think() of a running mission
    bool running = false;              // the last think() saw a running mission (gameplay and mode word 1)
};
Bot g_bot;

std::string env_string(const char *name) { const char *t = std::getenv(name); return t ? t : ""; }

void parse_params(Params &p, const std::string &spec) {
    std::size_t pos = 0;
    while (pos < spec.size()) {
        std::size_t end = spec.find(',', pos);
        if (end == std::string::npos) end = spec.size();
        const std::string item = spec.substr(pos, end - pos);
        const std::size_t eq = item.find('=');
        if (eq != std::string::npos) {
            const std::string k = item.substr(0, eq);
            const float v = static_cast<float>(std::strtod(item.c_str() + eq + 1, nullptr));
            if (k == "approach") p.approach = v;
            else if (k == "range") p.fire_range = v;
            else if (k == "cone") p.aim_cone = v;
            else if (k == "lock") p.lock = static_cast<int>(v);
            else if (k == "prim") p.prim = static_cast<std::uint32_t>(v);
            else if (k == "sec") p.sec = static_cast<std::uint32_t>(v);
            else if (k == "pulse") p.pulse = static_cast<int>(v);
            else if (k == "pulse2") p.pulse2 = static_cast<int>(v);
            else if (k == "lock2") p.lock2 = static_cast<int>(v);
            else if (k == "sec_after") p.sec_after = static_cast<int>(v);
            else if (k == "back") p.back = v;
            else if (k == "fresh") p.fresh = static_cast<int>(v);
            else if (k == "phase") p.phase = static_cast<int>(v);
            else if (k == "retry") p.retry = static_cast<int>(v);
            else if (k == "verbose") p.verbose = static_cast<int>(v);
        }
        pos = end + 1;
    }
}

struct Npc {
    std::uint32_t ptr;
    float x, z, hp, thr;
    unsigned st;
    std::uint32_t vt;
};

constexpr std::uint32_t kInfectedVtable = 0x08ACE704u;
constexpr std::uint32_t kWorldSingleton = 0x08AD0450u;

std::uint8_t stick(float v) {  // -1..1 -> 0..255 around 128
    const float c = std::fmax(-1.0f, std::fmin(1.0f, v));
    return static_cast<std::uint8_t>(std::lround(128.0f + c * 127.0f));
}

void think(std::uint64_t flip) {
    Bot &b = g_bot;
    b.flip = flip;
    b.alive = 0u;
    b.running = false;
    PadSample pad;  // neutral
    if (!in_ram(b.player) || rd32(b.player + 0x14u) != kPlayerVtable) {
        if (flip % 30u == 0u) b.player = find_player(); else b.player = 0u;
    }
    const std::uint32_t world = rd32(kWorldSingleton);
    if (world == 0u) { b.world_since = 0u; b.playing_since = 0u; }
    else if (b.world_since == 0u) b.world_since = flip;
    const std::uint32_t mode = rd32(kPausedMode);
    const bool gameplay = b.player != 0u && world != 0u;
    {   // state transitions
        static int last_state = -1;
        const int state = (gameplay ? 1 : 0) | (static_cast<int>(mode) << 1) | (world != 0u ? 64 : 0);
        if (state != last_state) {
            std::fprintf(stderr, "[bot] flip %llu state gameplay=%d mode=%u world=%08x player=%08x emu_us %llu\n", static_cast<unsigned long long>(flip), gameplay ? 1 : 0, mode, world, b.player,
                         static_cast<unsigned long long>(portablekit::kernel().now_us()));
            last_state = state;
        }
    }
    if (!gameplay) {
        // menus, movies, loading: press CROSS now and then (title, main menu, mission selection, briefing, results all take it)
        const std::uint64_t t = (flip + static_cast<std::uint64_t>(b.par.phase)) % 100u;
        pad.buttons = t < 6u ? kCross : 0u;
        if (b.par.fresh && flip >= 2000u && flip < 2500u) {
            const std::uint64_t k = (flip - 2000u) / 30u, u = (flip - 2000u) % 30u;
            pad.buttons = 0u;
            if (u < 6u) pad.buttons = k < 8u ? kRight : (k < 14u ? kDown : 0u);
            if (k >= 14u && flip >= 2480u && flip < 2486u) pad.buttons = kCross;
        }
        b.out = pad;
        return;
    }
    if (mode != 1u) {
        // a dialog, the briefing or the results screen
        if (b.modal_since == 0u) b.modal_since = flip;
        const std::uint64_t t = (flip - b.modal_since) % 240u;
        if (b.playing_since == 0u) pad.buttons = ((flip + static_cast<std::uint64_t>(b.par.phase)) % 100u) < 6u ? kCross : 0u;  // briefing: accept
        else if (t < 6u) pad.buttons = kTriangle;                                                // tutorial dialog: RETURN TO GAME
        else if (b.par.retry && t >= 100u && t < 106u) pad.buttons = kLeft;                      // results: highlight RETRY
        else if (t >= 150u && t < 156u) pad.buttons = kCross;                                    // results: ACCEPT
        b.out = pad;
        return;
    }
    b.modal_since = 0u;
    b.running = true;
    if (b.playing_since == 0u) b.playing_since = flip;
    const std::uint32_t body = rd32(b.player + 0x40Cu);
    const float px = rdf(body + 0x30u), pz = rdf(body + 0x38u), hx = rdf(body + 0x48u), hz = rdf(body + 0x50u);
    const float heading = std::atan2(hz, hx);
    std::vector<Npc> threats;
    for (std::uint32_t npc : live_npcs()) {
        const std::uint32_t nbody = rd32(npc + 0x40Cu);
        if (!in_ram(nbody)) continue;
        Npc n{npc, rdf(nbody + 0x30u), rdf(nbody + 0x38u), rdf(npc + 0x31Cu), rdf(npc + 0x320u), static_cast<unsigned>(rd8(npc + 0x58u) | (rd8(npc + 0x59u) << 8)), rd32(npc + 0x14u)};
        if (n.vt != kInfectedVtable) continue;
        if (n.st & 0x200u) continue;         // dead (bit 0x200, to be verified)
        if (!(n.hp > 0.0f) && !(n.st & 0x100u)) continue;
        threats.push_back(n);
    }
    b.alive = static_cast<unsigned>(threats.size());
    const Npc *target = nullptr;
    float best = 1e30f, tdist = 0.0f, terr = 0.0f;
    for (const Npc &n : threats) {
        const float dx = n.x - px, dz = n.z - pz, d = std::sqrt(dx * dx + dz * dz);
        if (d < best) { best = d; target = &n; tdist = d; terr = std::atan2(dz, dx) - heading; }
    }
    if (target == nullptr) {
        // nothing to shoot: walk down the street (+x); when blocked (no movement for 45 flips) turn by a pseudo-random amount, walk 200 flips along
        // the new heading, then resume (a crude explorer: the bot has no map)
        static float last_x = 0.0f, last_z = 0.0f, turn_dir = 1.0f;
        static std::uint64_t last_move = 0, phase_until = 0;
        static int phase = 0;
        static std::uint32_t lcg = 12345u;
        if (std::hypot(px - last_x, pz - last_z) > 1.0f) { last_move = flip; last_x = px; last_z = pz; }
        if (phase == 0 && flip - last_move > 45u) {
            lcg = lcg * 1664525u + 1013904223u;
            turn_dir = (lcg >> 16) & 1u ? 1.0f : -1.0f;
            phase = 1;
            phase_until = flip + 20u + ((lcg >> 8) % 50u);
            last_move = flip;
        }
        if (phase == 1) {
            pad.analog_x = turn_dir > 0 ? 255u : 0u;
            pad.analog_y = 128u;
            if (flip >= phase_until) { phase = 2; phase_until = flip + 200u; last_move = flip; }
        } else if (phase == 2) {
            pad.analog_y = stick(-1.0f);
            if (flip >= phase_until) { phase = 0; last_move = flip; }
        } else {
            float e = std::atan2(0.0f, 1.0f) - heading;
            e = std::remainder(e, 6.2831853f);
            pad.analog_x = stick(e * 57.2958f / 12.0f);
            pad.analog_y = stick(-1.0f);
        }
        b.out = pad;
        return;
    }
    terr = std::remainder(terr, 6.2831853f);
    const float err_deg = terr * 57.2958f;
    const bool weak = (target->st & 0x100u) != 0u;  // red: hp reached 0 under pistol fire, only the viral gun finishes it
    float lx = err_deg / 12.0f;
    if (std::fabs(err_deg) > 1.0f && std::fabs(lx) < 0.45f) lx = lx < 0 ? -0.45f : 0.45f;
    pad.analog_x = stick(lx);
    pad.analog_y = tdist > b.par.approach ? stick(-1.0f) : (tdist < b.par.back ? stick(1.0f) : 128u);
    if (b.par.lock && tdist < b.par.fire_range + 60.0f) pad.buttons |= kR;
    if (weak && b.par.lock2) pad.buttons |= kR;
    {
        const int pl = weak ? b.par.pulse2 : b.par.pulse;
        if (std::fabs(err_deg) < b.par.aim_cone && tdist < b.par.fire_range && (!weak || static_cast<int>(flip) >= b.par.sec_after) && (pl == 0 || (flip / static_cast<std::uint64_t>(pl)) % 2u == 0u)) pad.buttons |= weak ? b.par.sec : b.par.prim;
    }
    b.out = pad;
    if (b.par.verbose && flip - b.last_log >= 10u) {
        b.last_log = flip;
        std::fprintf(stderr, "[bot] flip %llu tgt %08x d %.0f e %+.0f hp %g/%g%s btn %04x lx %u ly %u\n", static_cast<unsigned long long>(flip), target->ptr, tdist, err_deg,
                     target->hp, target->thr, weak ? " WEAK" : "", pad.buttons, pad.analog_x, pad.analog_y);
    }
}


// ---- observer: mission statistics and events, read from guest memory (no writes) ----------------------------------
// Global 0x08ACE588 -> score-board object (vtable word 0x08ACE95C at +0x1C); its statistics block sits at +0x128:
// +0 score, +0xC infected killed, +0x10 ground combos, +0x14 best ground combo, +0x20 viral shots, +0x24 mission seconds,
// +0x28 hazard pay, +0x2C medal (0xFFFFFFFF while the mission runs, 1 bronze, 2 silver, ...).
constexpr std::uint32_t kStatsGlobal = 0x08ACE588u, kStatsVtable = 0x08ACE95Cu, kStatsOffset = 0x128u;
struct Stats {
    bool valid = false;
    std::uint32_t object = 0, score = 0, kills = 0, ground_combos = 0, best_combo = 0, shots = 0, seconds = 0, hazard = 0, medal = 0;
};
Stats read_stats() {
    Stats s;
    const std::uint32_t o = rd32(kStatsGlobal);
    if (!in_ram(o) || !in_ram(o + kStatsOffset + 0x30u) || rd32(o + 0x1Cu) != kStatsVtable) return s;
    const std::uint32_t b = o + kStatsOffset;
    s = {true, o, rd32(b), rd32(b + 0xCu), rd32(b + 0x10u), rd32(b + 0x14u), rd32(b + 0x20u), rd32(b + 0x24u), rd32(b + 0x28u), rd32(b + 0x2Cu)};
    return s;
}

struct Observer {
    bool on = false;
    bool complete = false;
    int missions_completed = 0;
    int deaths = 0;
    bool dead = false;
    bool results_seen = false;
    std::uint32_t last_object = 0;
};
Observer g_obs;

void observe(std::uint64_t flip) {
    Observer &o = g_obs;
    if (!o.on) return;
    const Stats s = read_stats();
    if (s.valid && s.object != o.last_object) {
        std::fprintf(stderr, "[bot] event stats_object flip %llu at %08x\n", static_cast<unsigned long long>(flip), s.object);
        o.last_object = s.object;
        o.complete = false;
    }
    const bool complete = s.valid && s.medal != 0xFFFFFFFFu && s.kills > 0u && s.hazard > 0u;  // hazard pay is 0 until the mission ends (medal alone can be stale after a RETRY)
    if (complete != o.complete) {
        o.complete = complete;
        if (complete) {
            ++o.missions_completed;
            std::fprintf(stderr, "[bot] event mission_complete flip %llu n %d score %u kills %u ground_combos %u best_combo %u viral_shots %u seconds %u hazard %u medal %u deaths %d\n",
                         static_cast<unsigned long long>(flip), o.missions_completed, s.score, s.kills, s.ground_combos, s.best_combo, s.shots, s.seconds, s.hazard, s.medal, o.deaths);
        }
    }
    if (complete && !o.results_seen && rd32(kPausedMode) != 1u) {
        o.results_seen = true;
        std::fprintf(stderr, "[bot] event results_screen flip %llu\n", static_cast<unsigned long long>(flip));
    }
    if (!complete) o.results_seen = false;
    if (flip % 600u == 0u) {
        // heap headroom (dlmalloc state 0x08ADD25C, +0xC = size of the top chunk) and the Npc pool (id 1): used, capacity
        const std::uint32_t pool = rd32(kPoolRegistry + 4u);
        std::fprintf(stderr, "[bot] stats flip %llu emu_us %llu score %u kills %u shots %u seconds %u medal %d heap_top %u npc_live %u npc_cap %u\n", static_cast<unsigned long long>(flip), static_cast<unsigned long long>(portablekit::kernel().now_us()), s.score, s.kills,
                     s.shots, s.seconds, s.valid ? static_cast<int>(s.medal) : -2, rd32(0x08ADD25Cu + 0xCu), static_cast<unsigned>(live_npcs().size()), in_ram(pool) ? rd32(pool + 0x68u) : 0u);
    }
    static std::uint32_t player = 0;
    if (!in_ram(player) || rd32(player + 0x14u) != kPlayerVtable) player = (flip % 30u == 0u) ? find_player() : 0u;
    if (in_ram(player)) {
        const bool dead = (rd8(player + 0x59u) & 2u) != 0u;
        if (dead != o.dead) {
            o.dead = dead;
            if (dead) {
                ++o.deaths;
                std::fprintf(stderr, "[bot] event player_dead flip %llu n %d\n", static_cast<unsigned long long>(flip), o.deaths);
            } else {
                std::fprintf(stderr, "[bot] event player_revived flip %llu\n", static_cast<unsigned long long>(flip));
            }
        }
    }
}

// ---- recorder: the bot's pad output as a pad script (the format of INFECTED_PAD_SCRIPT / pad_script_file) -----------
std::FILE *g_record = nullptr;
bool g_record_have = false;
PadSample g_record_last;
void record(std::uint64_t flip, const PadSample &pad) {
    if (g_record == nullptr) return;
    if (g_record_have && pad.buttons == g_record_last.buttons && pad.analog_x == g_record_last.analog_x && pad.analog_y == g_record_last.analog_y) return;
    g_record_have = true;
    g_record_last = pad;
    static const struct { std::uint32_t bit; const char *name; } kNames[] = {{0x0001u, "SELECT"}, {0x0008u, "START"}, {0x0010u, "UP"}, {0x0020u, "RIGHT"}, {0x0040u, "DOWN"}, {0x0080u, "LEFT"},
                                                                          {0x0100u, "L"}, {0x0200u, "R"}, {0x1000u, "TRIANGLE"}, {0x2000u, "CIRCLE"}, {0x4000u, "CROSS"}, {0x8000u, "SQUARE"}};
    std::string line = std::to_string(flip) + ":";
    bool any = false;
    for (const auto &n : kNames)
        if (pad.buttons & n.bit) { line += (any ? "+" : "") + std::string(n.name); any = true; }
    if (pad.analog_x != 0x80u || pad.analog_y != 0x80u) {
        line += (any ? "+" : "") + std::string("LX=") + std::to_string(pad.analog_x) + "+LY=" + std::to_string(pad.analog_y);
        any = true;
    }
    if (!any) line += "-";
    std::fprintf(g_record, "%s\n", line.c_str());
    std::fflush(g_record);
}

bool provide_pad(PadSample &out) {
    out = g_bot.out;
    return true;
}

// INFECTED_BOT_HOOKS=addr,addr: log every call of those guest functions (flip, a0..a3, f12) and run the original lean code
std::uint64_t g_flip_now = 0;
struct Hooked { std::uint32_t addr; psprecomp::LeanEntry original; };
Hooked g_hooked[8];
int g_hooked_count = 0;

template <int I>
void hook_thunk(psprecomp::CorpusRuntime &, psprecomp::AllegrexContext &ctx) {
    float f12;
    std::memcpy(&f12, &ctx.fpr[12], 4);
    std::fprintf(stderr, "[hook] flip %llu %08x a0 %08x a1 %08x a2 %08x a3 %08x f12 %g\n", static_cast<unsigned long long>(g_flip_now), g_hooked[I].addr, ctx.gpr[4], ctx.gpr[5],
                 ctx.gpr[6], ctx.gpr[7], f12);
    if (g_hooked[I].original != nullptr) g_hooked[I].original();
    ctx.pc = ctx.gpr[31];
}
using ThunkFn = void (*)(psprecomp::CorpusRuntime &, psprecomp::AllegrexContext &);
const ThunkFn kThunks[8] = {&hook_thunk<0>, &hook_thunk<1>, &hook_thunk<2>, &hook_thunk<3>, &hook_thunk<4>, &hook_thunk<5>, &hook_thunk<6>, &hook_thunk<7>};

void install_hooks(psprecomp::Runtime &runtime) {
    const std::string spec = [] { const char *t = std::getenv("INFECTED_BOT_HOOKS"); return std::string(t ? t : ""); }();
    std::size_t pos = 0;
    while (pos < spec.size() && g_hooked_count < 8) {
        std::size_t end = spec.find(',', pos);
        if (end == std::string::npos) end = spec.size();
        const std::uint32_t a = static_cast<std::uint32_t>(std::strtoul(spec.c_str() + pos, nullptr, 16));
        if (a != 0u) {
            g_hooked[g_hooked_count] = {a, psprecomp::exec_lean_entry(a)};
            runtime.register_function(a, kThunks[g_hooked_count], "bot_hook");
            std::fprintf(stderr, "[bot] hooked %08x\n", a);
            ++g_hooked_count;
        }
        pos = end + 1;
    }
}

} // namespace

void install(psprecomp::Runtime &runtime) {
    install_hooks(runtime);
    const std::string mode = env_string("INFECTED_BOT");
    g_obs.on = (!mode.empty() && mode != "0") || !env_string("INFECTED_BOT_OBSERVE").empty();
    if (mode.empty() || mode == "0") return;
    if (const std::string rec = env_string("INFECTED_BOT_RECORD"); !rec.empty()) {
        g_record = std::fopen(rec.c_str(), "w");
        if (g_record != nullptr) std::fprintf(g_record, "# pad log written by infected_bot (INFECTED_BOT_RECORD): flip then buttons, LX/LY\n");
    }
    g_bot.enabled = true;
    parse_params(g_bot.par, env_string("INFECTED_BOT_PARAMS"));
    portablekit::set_pad_provider(&provide_pad);
    std::fprintf(stderr, "[bot] enabled (mode %s)\n", mode.c_str());
}

Status status() {
    Status s;
    s.enabled = g_bot.enabled;
    s.playing = g_bot.running;
    s.results = g_obs.results_seen;
    s.infected_alive = g_bot.alive;
    s.missions_completed = static_cast<unsigned>(g_obs.missions_completed);
    s.deaths = static_cast<unsigned>(g_obs.deaths);
    return s;
}

void frame(psprecomp::Runtime &, std::uint64_t flip) {
    g_flip_now = flip;
    probe(flip);
    snap(flip);
    table(flip);
    if (g_bot.enabled) { think(flip); record(flip, g_bot.out); }
    observe(flip);
}

} // namespace infected::bot
