// Host test of the Modern button layout (app/src/pad_layout.h). Build and run on any machine:
//   g++ -O2 -std=c++17 -I../app/src t_pad_layout_host.cpp -o t_pad_layout_host && ./t_pad_layout_host
#include "pad_layout.h"

#include <stdio.h>

using namespace padlayout;

static int bad = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); bad++; } } while (0)

static const uint32_t kSelect = 0x1, kStart = 0x8, kUp = 0x10, kSquare = 0x8000, kCircle = 0x2000, kTriangle = 0x1000;

int main() {
    State st;
    // Classic: everything passes through, whatever the stick says.
    CHECK(apply(st, false, true, kLTrigger | kRTrigger | kCross, 0, 1000) == (kLTrigger | kRTrigger | kCross));
    CHECK(apply(st, false, true, 0, 128, 2000) == 0);

    // Modern, idle: nothing.
    CHECK(apply(st, true, true, 0, 128, 3000) == 0);
    // Cross, Square, Circle, Triangle, D-pad, START, SELECT keep their meaning.
    const uint32_t keep = kCross | kSquare | kCircle | kTriangle | kUp | kStart | kSelect;
    CHECK(apply(st, true, true, keep, 128, 4000) == keep);
    // L1 -> R, and L is not produced by it.
    CHECK(apply(st, true, true, kLTrigger, 128, 5000) == kRTrigger);
    // The right stick up or down opens the map; sideways and the dead zone do not.
    CHECK(apply(st, true, true, 0, 0, 6000) == kLTrigger);
    CHECK(apply(st, true, true, 0, 255, 6000) == kLTrigger);
    CHECK(apply(st, true, true, 0, 40, 6000) == kLTrigger);
    CHECK(apply(st, true, true, 0, 41, 6000) == 0);
    CHECK(apply(st, true, true, 0, 215, 6000) == 0);
    CHECK(apply(st, true, true, 0, 216, 6000) == kLTrigger);
    // Lock on and the map together: R and L.
    CHECK(apply(st, true, true, kLTrigger, 0, 7000) == (kRTrigger | kLTrigger));

    // R1 without auto-fire: Cross for as long as it is held, and R is not passed on.
    State a;
    for (uint64_t t = 100000; t < 500000; t += 8333) CHECK(apply(a, true, false, kRTrigger, 128, t) == kCross);
    CHECK(apply(a, true, false, 0, 128, 600000) == 0);

    // R1 with auto-fire: starts with a shot, then 50 ms up, 50 ms down...; ten shots in a second.
    State f;
    const uint64_t t0 = 1000000;
    int edges = 0;
    bool last = false;
    uint32_t first = apply(f, true, true, kRTrigger, 128, t0);
    CHECK(first == kCross);
    for (uint64_t t = t0; t < t0 + 1000000; t += 1000) {
        const bool down = (apply(f, true, true, kRTrigger, 128, t) & kCross) != 0;
        if (down && !last) edges++;
        last = down;
        CHECK((apply(f, true, true, kRTrigger, 128, t) & kRTrigger) == 0);
    }
    CHECK(edges == 10);   // the first press is already down at t0, so 10 down-edges are counted after it starts (t0 itself counts as the first)
    // Releasing R1 resets the phase: the next press fires at once.
    CHECK(apply(f, true, true, 0, 128, t0 + 1234567) == 0);
    CHECK(apply(f, true, true, kRTrigger, 128, t0 + 1234567 + 20000) == kCross);
    // Cross held by the player together with R1 stays down through the auto-fire gaps.
    State g;
    for (uint64_t t = 2000000; t < 2300000; t += 5000) CHECK((apply(g, true, true, kRTrigger | kCross, 128, t) & kCross) != 0);

    // A clock that starts at 0 (the first read of a session) must not wedge the phase.
    State z;
    CHECK(apply(z, true, true, kRTrigger, 128, 0) == kCross);
    CHECK(apply(z, true, true, kRTrigger, 128, 60000) == 0);
    CHECK(apply(z, true, true, kRTrigger, 128, 110000) == kCross);

    printf(bad ? "FAILED (%d)\n" : "OK\n", bad);
    return bad ? 1 : 0;
}
