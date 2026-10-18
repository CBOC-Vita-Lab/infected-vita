// pad_layout.h - the "Modern" button layout: what the Vita's buttons mean to a game that only knows the PSP's.
// Header only and free of platform calls, so the app's pad reader (app_platform_hooks.cpp) and the host test
// (selftest/t_pad_layout_host.cpp) run the same code.
//
// The game reads Cross for the pistol, Square for the viral gun, R to lock on and strafe, L to open the map. That is a PSP
// layout: both triggers sit under the index fingers, so firing means taking a thumb off the stick. "Modern" is the shooter
// convention on a handheld that has two shoulder buttons and a second stick:
//   R1 (Vita R)  -> fire (PSP Cross), repeating while held when auto-fire is on
//   L1 (Vita L)  -> lock on and strafe (PSP R)
//   right stick up or down -> the map (PSP L), because the game has no camera to turn and the stick is otherwise unused
// Everything else (Cross, Square, Circle, Triangle, the D-pad, START, SELECT, the left stick) is left alone, so the old Cross
// still fires. "Classic" returns the buttons unchanged.
#pragma once

#include <stdint.h>

namespace padlayout {

// PSP bit layout (the same as plat::PadButton; repeated here so the header stands alone).
constexpr uint32_t kLTrigger = 0x00000100u;
constexpr uint32_t kRTrigger = 0x00000200u;
constexpr uint32_t kCross = 0x00004000u;

constexpr uint64_t kAutoFireHalfPeriodUs = 50000;   // 50 ms down, 50 ms up: ten shots a second, the rate the game takes a press per shot at
constexpr uint8_t kStickLow = 40;                   // right stick y at or below this (up) or at or above kStickHigh (down) is a flick
constexpr uint8_t kStickHigh = 216;

struct State {
    bool fire_down = false;       // R1 was down at the last read
    uint64_t fire_since_us = 0;   // when it went down: the phase of the auto-fire
};

// `raw`: the Vita's buttons as PSP bits. `right_y`: 0..255 (128 centre, 0 up). Returns what the game is shown.
inline uint32_t apply(State& st, bool modern, bool auto_fire, uint32_t raw, uint8_t right_y, uint64_t now_us) {
    if (!modern) {
        st.fire_down = false;
        return raw;
    }
    uint32_t out = raw & ~(kLTrigger | kRTrigger);
    if (raw & kLTrigger) out |= kRTrigger;
    if (right_y <= kStickLow || right_y >= kStickHigh) out |= kLTrigger;
    if (raw & kRTrigger) {
        if (!st.fire_down) {
            st.fire_down = true;
            st.fire_since_us = now_us;
        }
        const uint64_t held = now_us >= st.fire_since_us ? now_us - st.fire_since_us : 0;
        if (!auto_fire || ((held / kAutoFireHalfPeriodUs) & 1u) == 0u) out |= kCross;
    } else {
        st.fire_down = false;
    }
    return out;
}

}  // namespace padlayout
