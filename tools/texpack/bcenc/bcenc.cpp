// A small C ABI over rgbcx (bc7enc_rdo, public domain / MIT) so pack.py can compress with a high quality
// BC1 / BC3 (DXT1 / DXT5) encoder through ctypes. build.sh fetches rgbcx and builds libbcenc.so; nothing of
// rgbcx is copied into this repository.
#include "rgbcx.h"

#include <cstdint>
#include <cstring>

extern "C" {

void bcenc_init() { rgbcx::init(); }

// rgba: width*height RGBA bytes; width and height are multiples of 4 (the caller pads). out receives
// (width/4)*(height/4) blocks of 8 bytes (BC1) or 16 bytes (BC3), rows of blocks top to bottom.
void bcenc_encode(const std::uint8_t *rgba, int width, int height, int bc3, int level, std::uint8_t *out) {
    const int bw = width / 4, bh = height / 4;
    const std::size_t block_bytes = bc3 != 0 ? 16u : 8u;
    std::uint8_t pixels[16 * 4];
    for (int by = 0; by < bh; ++by)
        for (int bx = 0; bx < bw; ++bx) {
            for (int y = 0; y < 4; ++y) std::memcpy(pixels + y * 16, rgba + ((by * 4 + y) * width + bx * 4) * 4, 16);
            std::uint8_t *dst = out + (static_cast<std::size_t>(by) * bw + bx) * block_bytes;
            if (bc3 != 0) rgbcx::encode_bc3(static_cast<std::uint32_t>(level), dst, pixels);
            else rgbcx::encode_bc1(static_cast<std::uint32_t>(level), dst, pixels, false, false);
        }
}

}
