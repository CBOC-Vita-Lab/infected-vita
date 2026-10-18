// app_platform_hooks.h - connects the Vita platform layer (plat::) to the seams the PortableKit runtime exposes:
//   KernelPlatform   realtime clock, idle sleep and wake-up        (host/kernel/kernel_platform.hpp)
//   PadProvider      sceCtrl reads                                  (host/input/pad_provider.hpp)
//   AudioOutput      the sink's playback device                     (host/audio/audio_sink.hpp)
#pragma once

namespace app {
// Call before install_system(): the kernel reads the platform clock while it installs.
void install_platform_hooks();
// Stops the audio pump (the sink closes the device itself; this is for the paths that never reach it).
void shutdown_platform_hooks();
}  // namespace app
