// selftest/t_ffmpeg_stub.cpp - desktop twin of t_ffmpeg.cpp (the FFmpeg here is the Vita cross-build).
#include "st_common.h"

namespace st {
void t9_ffmpeg() { skip("T9", "ffmpeg", "the minimal FFmpeg is only built for the Vita (ARMv7)"); }
}  // namespace st
