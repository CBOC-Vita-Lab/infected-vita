// app_fatal.h - the fatal-error screen: what went wrong, the guest PC when there is one, "Press START to exit".
#pragma once

#include <stdint.h>

#include <string>

namespace app {

// Logs the error (with a flushed log), shows it on screen and waits for START (or leaves at once with fatal_wait=0 in config.txt,
// which unattended runs use), then exits the process. Never returns.
[[noreturn]] void fatal(const char* title, const std::string& message, bool has_guest_pc = false, uint32_t guest_pc = 0);

// std::terminate (an exception nobody caught) and the C++ runtime's own aborts end in fatal().
void install_terminate_handler();

}  // namespace app
