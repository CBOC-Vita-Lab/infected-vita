// app_stdio_lock.h - one lock around the C stdio calls that write and the calls that open and close files.
//
// newlib's FILE has no working lock in this build: two threads in printf on the same stream (the audio thread and the GL thread
// both log through PortableKit's printf and fprintf(stderr)) corrupted the heap, and two threads in fopen can be handed the same
// free FILE. The link wraps the functions listed in CMakeLists.txt (-Wl,--wrap=...) and each wrapper takes one recursive lock
// before it calls the real function. This covers every caller: PortableKit, FFmpeg, vitaGL, libstdc++ (ifstream opens through fopen).
// The platform log (plat::logf) does not use stdio, so it never waits on this lock.
// config.txt stdio_lock=0 and fopen_lock=0 turn the two halves off (the wrappers then call straight through).
#pragma once

#include <stdint.h>

namespace app {
struct StdioLockStats {
    uint64_t locked_calls = 0;   // write calls that took the lock
    uint64_t open_calls = 0;     // fopen / freopen / fdopen / fclose
    uint32_t max_wait_us = 0;    // the longest a caller waited for the lock
    uint32_t max_hold_open_us = 0;   // the longest an open or close held it
};
void stdio_lock_configure(bool stdio_on, bool fopen_on);
StdioLockStats stdio_lock_stats();
}  // namespace app
