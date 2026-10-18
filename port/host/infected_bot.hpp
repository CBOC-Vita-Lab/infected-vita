// The Infected verification bot: reads the game's state from guest memory and drives the pad (PadProvider), see tools/bot/README.md.
// Off unless INFECTED_BOT is set. Development probes (INFECTED_BOT_PROBE) only read memory.
#pragma once

#include <cstdint>

namespace psprecomp {
class Runtime;
}

namespace infected::bot {

void install(psprecomp::Runtime &runtime);                       // from GameProfile::patch_loaded_image
void frame(psprecomp::Runtime &runtime, std::uint64_t flip);     // from GameProfile::frame_hook (once per guest flip)

// What the bot saw at the last flip (read by the Vita's bench=play report; all zero while the bot is off).
struct Status {
    bool enabled{};            // INFECTED_BOT is set (config.txt key `bot`)
    bool playing{};            // a mission is running (engine mode 1, player and world present)
    bool results{};            // the results screen of a finished mission is up
    unsigned infected_alive{}; // infected the bot counts as threats (alive or red, not dead) in the current frame
    unsigned missions_completed{};
    unsigned deaths{};
};
[[nodiscard]] Status status();

} // namespace infected::bot
