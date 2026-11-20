// PC test of the boot screen's pictures (app/src/boot_art_core.cpp): decoding, enlargement, the cache file and the composed picture.
// Everything here is made up: the "disc art" is a drawn gradient with a disc and a ring, never a picture of a game.
//   build and run: vita/scripts/test-host.sh <PortableKit checkout>        (needs stb_image.h and stb_image_write.h from its third_party)
#include <math.h>
#include <stdio.h>
#include <string.h>

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "../app/src/boot_art_core.h"

using namespace app::boot_art;

static int g_failed = 0;
static void check(bool ok, const char* what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_failed++;
}

static Image make_art(int w, int h) {
    Image im;
    im.w = w;
    im.h = h;
    im.px.resize((size_t)w * h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const double fx = (double)x / w, fy = (double)y / h;
            const double d = hypot(fx - 0.55, fy - 0.5);
            int r = (int)(40 + 150 * fx), g = (int)(60 + 120 * fy), b = (int)(200 - 100 * fx * fy);
            if (d < 0.2) { r = 230; g = 200 - (int)(300 * d); b = 80; }
            if (fabs(d - 0.3) < 0.01) { r = g = b = 250; }
            im.px[(size_t)y * w + x] = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | 0xFF000000u;
        }
    return im;
}

static std::vector<uint8_t> to_png(const Image& im) {
    std::vector<uint8_t> out;
    stbi_write_png_to_func([](void* c, void* d, int n) { auto* v = (std::vector<uint8_t>*)c; v->insert(v->end(), (uint8_t*)d, (uint8_t*)d + n); }, &out, im.w,
                           im.h, 4, im.px.data(), im.w * 4);
    return out;
}

static double psnr(const Image& a, const Image& b) {
    double se = 0;
    for (size_t i = 0; i < a.px.size(); i++)
        for (int c = 0; c < 3; c++) {
            const double d = (double)((a.px[i] >> (8 * c)) & 255) - (double)((b.px[i] >> (8 * c)) & 255);
            se += d * d;
        }
    se /= (double)a.px.size() * 3;
    return se == 0 ? 99.0 : 10.0 * log10(255.0 * 255.0 / se);
}

// 2x box reduction: what a 480x272 picture is to the 960x544 one it stands for.
static Image halve(const Image& in) {
    Image o;
    o.w = in.w / 2;
    o.h = in.h / 2;
    o.px.resize((size_t)o.w * o.h);
    for (int y = 0; y < o.h; y++)
        for (int x = 0; x < o.w; x++) {
            int s[3] = {0, 0, 0};
            for (int k = 0; k < 4; k++) {
                const uint32_t p = in.px[(size_t)(2 * y + k / 2) * in.w + 2 * x + k % 2];
                for (int c = 0; c < 3; c++) s[c] += (p >> (8 * c)) & 255;
            }
            o.px[(size_t)y * o.w + x] = (uint32_t)((s[0] + 2) / 4) | ((uint32_t)((s[1] + 2) / 4) << 8) | ((uint32_t)((s[2] + 2) / 4) << 16) | 0xFF000000u;
        }
    return o;
}

int main() {
    check(crc32((const uint8_t*)"123456789", 9) == 0xCBF43926u, "crc32 of the check string");

    const Image big = make_art(960, 544);
    const Image small = halve(big);
    Image cubic, linear;
    resize_bicubic(small, 960, 544, &cubic);
    resize_bilinear(small, 960, 544, &linear);
    check(cubic.w == 960 && cubic.h == 544 && !cubic.empty(), "480x272 becomes 960x544");
    const double pc = psnr(big, cubic), pl = psnr(big, linear);
    printf("     PSNR against the original: bicubic %.2f dB, bilinear %.2f dB\n", pc, pl);
    check(pc > pl, "bicubic is closer to the original than bilinear on smooth, photographic-like art");

    // Flat colour stays flat (the weights add up to one), alpha stays alpha, a transparent logo gets no dark fringe.
    Image flat;
    flat.w = 16; flat.h = 9;
    flat.px.assign(16 * 9, 0xFF3366CCu);
    Image flat2;
    resize_bicubic(flat, 64, 36, &flat2);
    bool same = true;
    for (uint32_t p : flat2.px) same = same && p == 0xFF3366CCu;
    check(same, "a flat colour is still that colour after the enlargement");
    Image logo;
    logo.w = 8; logo.h = 8;
    logo.px.assign(64, 0u);
    for (int y = 2; y < 6; y++) for (int x = 2; x < 6; x++) logo.px[(size_t)y * 8 + x] = 0xFFFFFFFFu;   // white square, rest transparent black
    Image logo2;
    resize_bicubic(logo, 16, 16, &logo2);
    bool fringe = false;
    for (uint32_t p : logo2.px) if ((p >> 24) > 0 && (p & 255) < 240) fringe = true;
    check(!fringe, "the edge of a white logo on transparent black has no grey fringe");

    // PNG in, art out.
    Image icon = make_art(144, 80);
    for (int y = 0; y < icon.h; y++) for (int x = 0; x < icon.w; x++) if (x < 10 || x > 130) icon.px[(size_t)y * icon.w + x] &= 0x00FFFFFFu;
    const auto pic1_png = to_png(small), icon_png = to_png(icon);
    Image dec;
    check(decode_png(pic1_png.data(), pic1_png.size(), &dec) && dec.w == 480 && dec.h == 272 && dec.px == small.px, "a PNG decodes to exactly its pixels");
    check(!decode_png(pic1_png.data(), pic1_png.size() / 2, &dec), "a cut-off PNG is refused");
    const uint8_t junk[64] = {1, 2, 3};
    check(!decode_png(junk, sizeof(junk), &dec) && !decode_png(nullptr, 0, &dec), "bytes that are not a PNG are refused");

    Art art;
    check(build(pic1_png.data(), pic1_png.size(), icon_png.data(), icon_png.size(), 960, 544, &art) && art.bg.w == 960 && art.bg.h == 544 && art.logo.w == 288 && art.logo.h == 160,
          "build: PIC1 to 960x544 and ICON0 to 288x160");
    check((art.bg.px[1000] >> 24) == 255, "the background is opaque");
    Art only_bg, only_logo, none;
    check(build(pic1_png.data(), pic1_png.size(), nullptr, 0, 960, 544, &only_bg) && only_bg.logo.empty(), "a disc with PIC1 and no ICON0 still gives the background");
    check(build(nullptr, 0, icon_png.data(), icon_png.size(), 960, 544, &only_logo) && only_logo.bg.empty() && !only_logo.logo.empty(), "ICON0 alone gives the logo");
    check(!build(nullptr, 0, nullptr, 0, 960, 544, &none), "neither file: no art");
    check(!build(junk, sizeof(junk), junk, sizeof(junk), 960, 544, &none), "files that are not PNGs: no art");
    Art other;
    build(icon_png.data(), icon_png.size(), pic1_png.data(), pic1_png.size(), 960, 544, &other);
    check(other.key != art.key, "another disc's pictures have another key");

    // The cache file.
    const auto file = serialize(art);
    Art back;
    check(parse(file.data(), file.size(), &back) && back.key == art.key && back.bg.px == art.bg.px && back.logo.px == art.logo.px, "the cache file round-trips");
    check(!parse(file.data(), file.size() - 1, &back) && !parse(file.data(), 10, &back) && !parse(junk, sizeof(junk), &back), "a short or foreign cache file is refused");
    auto bad = file;
    bad[8] = 0xFF; bad[9] = 0xFF;   // an absurd width
    check(!parse(bad.data(), bad.size(), &back), "a cache file with an absurd size is refused");
    auto padded = file;
    padded.push_back(0);
    check(!parse(padded.data(), padded.size(), &back), "a cache file with trailing bytes is refused");

    // The composed screen.
    Image screen;
    int bottom = 0;
    compose(art, 55, &screen, &bottom);
    check(screen.w == 960 && screen.h == 544 && bottom == 20 + 160, "compose: full size, the logo ends at its row");
    auto lum = [](uint32_t p) { return (int)(p & 255) + (int)((p >> 8) & 255) + (int)((p >> 16) & 255); };
    check(lum(screen.px[300 * 960 + 100]) < lum(art.bg.px[300 * 960 + 100]), "the background is dimmed");
    check(lum(screen.px[540 * 960 + 100]) * 100 / (lum(art.bg.px[540 * 960 + 100]) + 1) < lum(screen.px[10 * 960 + 100]) * 100 / (lum(art.bg.px[10 * 960 + 100]) + 1), "and darker at the bottom than at the top");
    check(screen.px[(20 + 80) * 960 + 480] != screen.px[(20 + 80) * 960 + 20], "the logo is drawn in the middle");
    Art empty_bg = only_logo;
    compose(empty_bg, 55, &screen, &bottom);
    check(screen.w == 960 && screen.px[0] == 0xFF201414u, "without a background the plain boot colour stays");

    if (g_failed) printf("%d failed\n", g_failed);
    return g_failed ? 1 : 0;
}
