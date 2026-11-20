// boot_art_core.h - the pictures of the boot screen, made from the player's own disc: PSP_GAME/PIC1.PNG (the 480x272 background of the
// PSP's game menu) and PSP_GAME/ICON0.PNG (the 144x80 icon), decoded, enlarged to the Vita's 960x544 and composed into one picture the
// start-up screen draws behind its text. Pure code (no platform layer), so the PC tests run it. Nothing of a game is in the program or the
// package: the pictures exist only on the player's console, in ux0:data/infected/art/.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <vector>

namespace app::boot_art {

// Pixels are 32-bit words, red in the low byte and alpha in the top one (the byte order of GL_RGBA, and of the overlay's buffer).
struct Image {
    int w = 0, h = 0;
    std::vector<uint32_t> px;
    bool empty() const { return w <= 0 || h <= 0 || px.size() != (size_t)w * (size_t)h; }
};

// A PNG (any bit depth the decoder knows, always RGBA out). False for anything else, including pictures over 2048 pixels a side.
bool decode_png(const uint8_t* data, size_t size, Image* out);

// Bicubic (Catmull-Rom) enlargement or reduction to ow x oh, in premultiplied alpha, so the edge of a logo does not get a dark fringe.
// For the photographic art of PIC1 it is sharper than bilinear and, unlike MMPX (made for pixel art, and only 2x), it does not invent
// edges. 480x272 -> 960x544 is exactly 2x.
void resize_bicubic(const Image& in, int ow, int oh, Image* out);
// The same with bilinear weights, kept for the comparison in the PC tests.
void resize_bilinear(const Image& in, int ow, int oh, Image* out);

struct Art {
    Image bg;          // 960x544, opaque
    Image logo;        // 2x the icon (288x160), with alpha; empty when the disc has none
    uint32_t key = 0;  // CRC-32 of the two PNG files: the art belongs to this disc
};

uint32_t crc32(const uint8_t* data, size_t size, uint32_t seed = 0);

// pic1/icon0 may be empty (size 0); with neither, false.
bool build(const uint8_t* pic1, size_t pic1_size, const uint8_t* icon0, size_t icon0_size, int out_w, int out_h, Art* art);

// The cache file: a header, then the two images raw. parse() checks every length before it reads.
std::vector<uint8_t> serialize(const Art& art);
bool parse(const uint8_t* data, size_t size, Art* art);

// The picture the screen draws: the background at `dim_percent` of its brightness, darker towards the bottom where the text and the bar
// are, and the logo centred near the top over it. Size of art.bg.
void compose(const Art& art, int dim_percent, Image* out, int* logo_bottom);

}  // namespace app::boot_art
