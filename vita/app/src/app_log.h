// app_log.h - stdout / stderr / std::cout / std::cerr / std::clog all end up in the platform logger (log.txt and sceClibPrintf).
#pragma once

namespace app {
// Replaces the C stdio streams of the calling thread's reent and the C++ iostream buffers. Call once, early, from the main thread.
void log_redirect_streams();
}  // namespace app
