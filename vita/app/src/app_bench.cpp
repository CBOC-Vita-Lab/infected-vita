#include "app_bench.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "app_config.h"
#include "app_memory.h"
#include "app_status.h"
#include "app_ui.h"
#include "platform.h"

#include "gpu/ge_dump.hpp"
#include "gpu/ge_state.hpp"
#include "gpu/gl/gl_renderer.hpp"
#include "profile.hpp"
#include "psprecomp/guest_memory.hpp"

namespace app {

std::string bench_mode() { return cfg_str("bench", ""); }

// ----------------------------------------------------------------------------------------------------------------- bench=cpu
void bench_cpu_setup() {
    // The framework's own bench (host/perf/bench.hpp): <prefix>_BENCH=flips:N[,from:F] times N flips from flip F by category (guest code,
    // GE walk, vertex decode, GL submission, present, idle, kernel, audio, movie decode, HLE per library) and prints the report, which
    // stdout -> log.txt carries. config.txt has `bench=cpu`, which would reach the framework as INFECTED_BENCH=cpu, so the key is replaced here.
    // INFECTED_MAX_FRAMES is only a safety net a little after the window.
    const long flips = cfg_int("bench_flips", 3600);
    const long from = cfg_int("bench_from", 0);
    char buf[64];
    snprintf(buf, sizeof(buf), "flips:%ld,from:%ld", flips, from);
    setenv("INFECTED_BENCH", buf, 1);
    snprintf(buf, sizeof(buf), "%ld", from + flips + 120);
    setenv("INFECTED_MAX_FRAMES", buf, 1);
    plat::logf("[bench] cpu: INFECTED_BENCH=%s (the threaded renderer is off in bench runs)", getenv("INFECTED_BENCH"));
}

void bench_cpu_report(const char* stop_reason) {
    plat::logf("[bench] cpu: run ended (%s); the report is the block of [bench] lines above (ms per flip by category, HLE per library, calls per flip)",
               stop_reason ? stop_reason : "");
}

// ----------------------------------------------------------------------------------------------------------------- bench=play
void bench_play_setup() {
    // The bot plays a whole mission with the normal game: the real clock, the threaded renderer, the settings as they are in settings.txt and
    // config.txt (nothing is changed here but the bot). `bot=0` in config.txt would still win. The bot starts a mission from the title screen
    // by pressing CROSS (and reads the game's memory; see tools/bot/README.md): it needs a profile on the memory stick already (ms0), see docs/history/release-candidate/HARDWARE-TEST.md.
    config_default("BOT", "1");
    // config.txt's `bench=play` also reaches the framework as INFECTED_BENCH, whose own bench (bench=cpu's engine) wants flips:N and turns the threaded
    // renderer off: this mode is the normal game, so that variable must not exist.
    unsetenv("INFECTED_BENCH");
    plat::logf("[bench] play: the bot plays; INFECTED_BOT=%s INFECTED_BOT_PARAMS=%s", getenv("INFECTED_BOT") ? getenv("INFECTED_BOT") : "",
               getenv("INFECTED_BOT_PARAMS") ? getenv("INFECTED_BOT_PARAMS") : "(defaults)");
}

void bench_play_start() {
    app::status::play_enable(cfg_int("bench_seconds", 420), cfg_int("bench_interval", 10));
}

// ----------------------------------------------------------------------------------------------------------------- bench=ge_replay
namespace {

struct Window {
    double walk_ms = 0, submit_ms = 0, present_ms = 0, flip_ms = 0;
    unsigned flips = 0;
    void clear() { *this = Window(); }
};

void report(const char* tag, const Window& w, const portablekit::gpu::GlStats& s, unsigned flips_total) {
    if (!w.flips) return;
    const double n = w.flips;
    plat::logf("[bench] ge_replay %s flips=%u | per flip: walk+decode %.2f ms, GL submission %.2f ms, present+swap %.2f ms, total %.2f ms (%.1f fps) | "
               "submits %llu gl_draws %llu tex miss %llu hit %llu uploaded %llu KiB",
               tag, flips_total, (w.walk_ms - w.submit_ms) / n, w.submit_ms / n, w.present_ms / n, w.flip_ms / n, 1000.0 * n / (w.flip_ms > 0 ? w.flip_ms : 1),
               (unsigned long long)s.submits, (unsigned long long)s.gl_draws, (unsigned long long)s.texture_misses, (unsigned long long)s.texture_hits,
               (unsigned long long)(s.bytes_uploaded / 1024));
}

}  // namespace

int bench_ge_replay() {
    using namespace portablekit;
    namespace gedump = gpu::gedump;
    const std::string rel = cfg_str("bench_dump", "dumps");
    char dir[256];
    plat::path_resolve(plat::Root::Data, rel.c_str(), dir, sizeof(dir));
    ui::boot_title("Infected: GE replay benchmark");
    ui::boot_line("dumps in %s", dir);
    std::string error;
    std::vector<std::uint64_t> flips;
    try {
        flips = gedump::list_flips(dir);
    } catch (const std::exception& e) {
        error = e.what();
    }
    if (flips.empty()) {
        ui::boot_line("no flip_*.ge dump found (%s). Record on the desktop with INFECTED_GE_DUMP=<dir>, copy the directory here.", error.c_str());
        plat::logf("[bench] ge_replay: nothing to replay in %s", dir);
        return 1;
    }
    ui::boot_line("%u flips: first %llu, last %llu", (unsigned)flips.size(), (unsigned long long)flips.front(), (unsigned long long)flips.back());

    psprecomp::ArenaProvider provider;
    provider.allocate = &arena_allocate;
    provider.release = &arena_release;
    psprecomp::set_arena_provider(provider);

    auto renderer = std::make_unique<gpu::GlRenderer>();
    gpu::GlSettings settings = renderer->settings();
    for (const std::string& name : gpu::gl_setting_names()) {
        std::string upper = name;
        for (char& c : upper) c = (char)toupper((unsigned char)c);
        if (const char* value = getenv(("INFECTED_GL_" + upper).c_str())) gpu::apply_gl_setting(settings, name, value);
    }
    renderer->set_settings(settings);
    if (!renderer->initialize(gpu::RendererConfig{}, error)) {
        ui::boot_line("renderer: %s", error.c_str());
        return 1;
    }
    plat::logf("[bench] renderer: %s", renderer->gl_description().c_str());

    gedump::Pack pack;
    if (!pack.open(std::filesystem::path(dir) / "pages.pack", error)) {
        ui::boot_line("%s", error.c_str());
        return 1;
    }

    const long limit = cfg_int("bench_flips", 0);              // 0: loop until START is pressed
    const long report_every = cfg_int("bench_report", 60);
    std::unique_ptr<psprecomp::GuestMemory> memory;
    gpu::GeState ge;
    Window window, total;
    unsigned replayed = 0;
    std::uint64_t transfers = 0;
    ui::boot_hide();
    bool stop = false;
    for (unsigned pass = 0; !stop; ++pass) {
        for (const std::uint64_t flip : flips) {
            gedump::FlipDump dump;
            if (!gedump::load_flip(dir, flip, dump, error)) {
                plat::logf("[bench] %s", error.c_str());
                return 1;
            }
            if (!memory) memory = std::make_unique<psprecomp::GuestMemory>(dump.ram_size);
            if (!gedump::restore_base(pack, dump, *memory)) {
                plat::logf("[bench] cannot restore the memory of flip %llu", (unsigned long long)flip);
                return 1;
            }
            if (!dump.ge_state.empty() && !ge.load_state(dump.ge_state.data(), dump.ge_state.size())) {
                plat::logf("[bench] cannot load the GE state of flip %llu", (unsigned long long)flip);
                return 1;
            }
            const uint64_t flip_start = plat::now_us();
            uint64_t walk_us = 0, submit_us = 0, present_us = 0;
            bool first_event = true;
            for (const gedump::Event& event : dump.events) {
                if (!first_event && !gedump::apply_event_memory(pack, event, dump, *memory)) {
                    plat::logf("[bench] cannot apply memory changes in flip %llu", (unsigned long long)flip);
                    return 1;
                }
                first_event = false;
                switch (event.kind) {
                case gedump::EventKind::ListRun: {
                    renderer->begin_display_list();
                    ge.set_draw_sink([&](const gpu::DrawCall& call) {
                        const uint64_t t = plat::now_us();
                        renderer->submit(call, *memory);
                        submit_us += plat::now_us() - t;
                    });
                    ge.set_transfer_sink([&](const gpu::BlockTransfer& transfer) {
                        renderer->read_back_framebuffer(transfer.source, *memory);
                        const std::size_t row_bytes = (std::size_t)transfer.width * transfer.bytes_per_pixel;
                        for (std::uint32_t row = 0; row < transfer.height; ++row) {
                            const std::uint32_t from = transfer.source + ((transfer.source_y + row) * transfer.source_stride + transfer.source_x) * transfer.bytes_per_pixel;
                            const std::uint32_t to = transfer.destination + ((transfer.destination_y + row) * transfer.destination_stride + transfer.destination_x) * transfer.bytes_per_pixel;
                            const std::uint8_t* source = memory->raw_pointer(from, row_bytes);
                            std::uint8_t* destination = memory->raw_pointer(to, row_bytes);
                            if (source == nullptr || destination == nullptr) return;
                            memmove(destination, source, row_bytes);
                        }
                        ++transfers;
                        renderer->begin_display_list();
                    });
                    const uint64_t walk_start = plat::now_us();
                    bool finished = false;
                    try {
                        ge.execute(*memory, event.b, event.c, finished);
                    } catch (const std::exception& failure) {
                        plat::logf("[bench] flip %llu: list aborted: %s", (unsigned long long)flip, failure.what());
                    }
                    walk_us += plat::now_us() - walk_start;
                    ge.set_draw_sink(nullptr);
                    ge.set_transfer_sink(nullptr);
                    break;
                }
                case gedump::EventKind::Picture: {
                    if (event.e != 3u || (event.a & 0x1F000000u) != 0x04000000u) break;
                    const std::size_t bytes = (std::size_t)event.d * event.c * 4u;
                    renderer->upload_frame(event.a | 0x04000000u, memory->raw_pointer(event.a, bytes), event.b, event.c, event.d);
                    break;
                }
                case gedump::EventKind::Flip: {
                    const gedump::DisplayInfo& d = dump.display;
                    const std::uint32_t address = (d.framebuffer & 0xFF000000u) == 0u ? (d.framebuffer | 0x04000000u) : d.framebuffer;
                    if ((address & 0x1F000000u) != 0x04000000u && d.pixel_format == 3u) {
                        const std::size_t bytes = (std::size_t)d.buffer_width * d.height * 4u;
                        renderer->upload_frame(address, memory->raw_pointer(address, bytes), d.width, d.height, d.buffer_width);
                    }
                    const uint64_t present_start = plat::now_us();
                    renderer->write_back_frame(*memory);
                    renderer->present(address);
                    present_us += plat::now_us() - present_start;
                    break;
                }
                }
            }
            const double flip_ms = (plat::now_us() - flip_start) / 1000.0;
            window.walk_ms += walk_us / 1000.0;
            window.submit_ms += submit_us / 1000.0;
            window.present_ms += present_us / 1000.0;
            window.flip_ms += flip_ms;
            window.flips++;
            total.walk_ms += walk_us / 1000.0;
            total.submit_ms += submit_us / 1000.0;
            total.present_ms += present_us / 1000.0;
            total.flip_ms += flip_ms;
            total.flips++;
            ++replayed;
            if (report_every > 0 && (long)window.flips >= report_every) {
                report("window", window, renderer->total_stats(), replayed);
                window.clear();
                memory_sample();
            }
            renderer->reset_frame_stats();
            plat::PadState pad;
            if (plat::pad_poll(&pad) && (pad.buttons & plat::PAD_START)) stop = true;
            if (limit > 0 && (long)replayed >= limit) stop = true;
            if (stop) break;
        }
        if (flips.size() == 1 && limit == 0 && pass > 100000) break;
    }
    report("window", window, renderer->total_stats(), replayed);
    report("total", total, renderer->total_stats(), replayed);
    const gpu::GlStats& t = renderer->total_stats();
    plat::logf("[bench] ge_replay totals over %u flips: submits %llu gl_draws %llu merged %llu vertices_in %llu uploaded %llu KiB texture decodes %llu "
               "revalidations %llu stale %llu transfers %llu shader variants %lu (compile+link %.0f ms) cached textures %lu (%llu KiB)",
               replayed, (unsigned long long)t.submits, (unsigned long long)t.gl_draws, (unsigned long long)t.merged, (unsigned long long)t.vertices_in,
               (unsigned long long)(t.bytes_uploaded / 1024), (unsigned long long)t.texture_misses, (unsigned long long)t.texture_checks,
               (unsigned long long)t.texture_stale, (unsigned long long)transfers, (unsigned long)renderer->shader_variants(), t.shader_compile_us / 1000.0,
               (unsigned long)renderer->cached_textures(), (unsigned long long)(renderer->cached_texture_bytes() / 1024));
    plat::logf("%s", memory_line().c_str());
    return 0;
}

}  // namespace app
