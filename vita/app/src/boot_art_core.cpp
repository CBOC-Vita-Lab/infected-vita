#include "boot_art_core.h"

#include <string.h>

#include <algorithm>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_MAX_DIMENSIONS 2048
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#pragma GCC diagnostic pop

namespace app::boot_art {
namespace {

inline uint32_t pack(int r, int g, int b, int a) { return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24); }
inline int ch(uint32_t p, int i) { return (int)((p >> (8 * i)) & 255u); }
inline int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

constexpr int kQ = 14;   // fixed point of the filter weights

struct Taps {
    int first = 0;
    int w[4] = {0, 0, 0, 0};
};

// One output coordinate's source pixels and weights. kind 0: Catmull-Rom (4 taps), 1: bilinear (2 taps in the middle of 4).
void make_taps(int in_n, int out_n, int kind, std::vector<Taps>* taps) {
    taps->resize((size_t)out_n);
    for (int o = 0; o < out_n; o++) {
        const double src = (o + 0.5) * (double)in_n / (double)out_n - 0.5;
        const int i0 = (int)(src >= 0 ? (int)src : (int)src - 1);   // floor
        const double t = src - i0;
        double wt[4];
        if (kind == 0) {
            const double t2 = t * t, t3 = t2 * t;
            wt[0] = -0.5 * t3 + t2 - 0.5 * t;
            wt[1] = 1.5 * t3 - 2.5 * t2 + 1.0;
            wt[2] = -1.5 * t3 + 2.0 * t2 + 0.5 * t;
            wt[3] = 0.5 * t3 - 0.5 * t2;
        } else {
            wt[0] = 0;
            wt[1] = 1.0 - t;
            wt[2] = t;
            wt[3] = 0;
        }
        Taps& tp = (*taps)[(size_t)o];
        tp.first = i0 - 1;
        int sum = 0;
        for (int k = 0; k < 4; k++) {
            tp.w[k] = (int)(wt[k] * (1 << kQ) + (wt[k] >= 0 ? 0.5 : -0.5));
            sum += tp.w[k];
        }
        tp.w[1] += (1 << kQ) - sum;   // the weights add up to exactly one
    }
}

// Premultiplied-alpha separable resize. The pass between the two is kept at 8 bits per channel (rounding error of at most half a step),
// which keeps the working memory at one image of the output's width and the input's height.
void resize(const Image& in, int ow, int oh, int kind, Image* out) {
    out->w = out->h = 0;
    out->px.clear();
    if (in.empty() || ow <= 0 || oh <= 0) return;
    std::vector<Taps> tx, ty;
    make_taps(in.w, ow, kind, &tx);
    make_taps(in.h, oh, kind, &ty);
    std::vector<uint32_t> mid((size_t)ow * (size_t)in.h);
    for (int y = 0; y < in.h; y++) {
        const uint32_t* row = &in.px[(size_t)y * in.w];
        for (int x = 0; x < ow; x++) {
            const Taps& t = tx[(size_t)x];
            int acc[4] = {0, 0, 0, 0};
            for (int k = 0; k < 4; k++) {
                if (t.w[k] == 0) continue;
                const int sx = std::clamp(t.first + k, 0, in.w - 1);
                const uint32_t p = row[sx];
                const int a = ch(p, 3);
                acc[0] += t.w[k] * (ch(p, 0) * a / 255);   // premultiplied
                acc[1] += t.w[k] * (ch(p, 1) * a / 255);
                acc[2] += t.w[k] * (ch(p, 2) * a / 255);
                acc[3] += t.w[k] * a;
            }
            for (int c = 0; c < 4; c++) acc[c] = clamp255((acc[c] + (1 << (kQ - 1))) >> kQ);
            mid[(size_t)y * ow + x] = pack(acc[0], acc[1], acc[2], acc[3]);
        }
    }
    out->w = ow;
    out->h = oh;
    out->px.assign((size_t)ow * (size_t)oh, 0u);
    for (int y = 0; y < oh; y++) {
        const Taps& t = ty[(size_t)y];
        for (int x = 0; x < ow; x++) {
            int acc[4] = {0, 0, 0, 0};
            for (int k = 0; k < 4; k++) {
                if (t.w[k] == 0) continue;
                const int sy = std::clamp(t.first + k, 0, in.h - 1);
                const uint32_t p = mid[(size_t)sy * ow + x];
                for (int c = 0; c < 4; c++) acc[c] += t.w[k] * ch(p, c);
            }
            for (int c = 0; c < 4; c++) acc[c] = clamp255((acc[c] + (1 << (kQ - 1))) >> kQ);
            const int a = acc[3];
            if (a > 0 && a < 255) {   // back to straight alpha
                for (int c = 0; c < 3; c++) acc[c] = std::min(255, (acc[c] * 255 + a / 2) / a);
            }
            out->px[(size_t)y * ow + x] = pack(acc[0], acc[1], acc[2], a);
        }
    }
}

void put32(std::vector<uint8_t>* v, uint32_t x) {
    for (int i = 0; i < 4; i++) v->push_back((uint8_t)(x >> (8 * i)));
}
uint32_t get32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

constexpr char kMagic[4] = {'I', 'B', 'A', '1'};

}  // namespace

bool decode_png(const uint8_t* data, size_t size, Image* out) {
    out->w = out->h = 0;
    out->px.clear();
    if (!data || size < 8 || size > (16u << 20)) return false;
    int w = 0, h = 0, n = 0;
    stbi_uc* img = stbi_load_from_memory(data, (int)size, &w, &h, &n, 4);
    if (!img) return false;
    out->w = w;
    out->h = h;
    out->px.resize((size_t)w * (size_t)h);
    memcpy(out->px.data(), img, out->px.size() * 4);
    stbi_image_free(img);
    return true;
}

void resize_bicubic(const Image& in, int ow, int oh, Image* out) { resize(in, ow, oh, 0, out); }
void resize_bilinear(const Image& in, int ow, int oh, Image* out) { resize(in, ow, oh, 1, out); }

uint32_t crc32(const uint8_t* data, size_t size, uint32_t seed) {
    static uint32_t table[256];
    static bool made = false;
    if (!made) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        made = true;
    }
    uint32_t c = ~seed;
    for (size_t i = 0; i < size; i++) c = table[(c ^ data[i]) & 255u] ^ (c >> 8);
    return ~c;
}

bool build(const uint8_t* pic1, size_t pic1_size, const uint8_t* icon0, size_t icon0_size, int out_w, int out_h, Art* art) {
    *art = Art();
    Image bg, logo;
    const bool have_bg = pic1_size != 0 && decode_png(pic1, pic1_size, &bg);
    const bool have_logo = icon0_size != 0 && decode_png(icon0, icon0_size, &logo);
    if (!have_bg && !have_logo) return false;
    if (have_bg) {
        resize_bicubic(bg, out_w, out_h, &art->bg);
        for (uint32_t& p : art->bg.px) p |= 0xFF000000u;   // the background is opaque
    }
    if (have_logo) {
        // 2x is the PSP's 144x80 at the Vita's 960 pixel width what the game menu's own scale is; bigger would look soft.
        const int scale = out_w >= 960 ? 2 : 1;
        resize_bicubic(logo, logo.w * scale, logo.h * scale, &art->logo);
    }
    art->key = crc32(icon0, icon0_size, crc32(pic1, pic1_size));
    return true;
}

std::vector<uint8_t> serialize(const Art& art) {
    std::vector<uint8_t> v;
    v.insert(v.end(), kMagic, kMagic + 4);
    put32(&v, art.key);
    put32(&v, (uint32_t)art.bg.w);
    put32(&v, (uint32_t)art.bg.h);
    put32(&v, (uint32_t)art.logo.w);
    put32(&v, (uint32_t)art.logo.h);
    const size_t at = v.size();
    v.resize(at + (art.bg.px.size() + art.logo.px.size()) * 4);
    if (!art.bg.px.empty()) memcpy(&v[at], art.bg.px.data(), art.bg.px.size() * 4);
    if (!art.logo.px.empty()) memcpy(&v[at + art.bg.px.size() * 4], art.logo.px.data(), art.logo.px.size() * 4);
    return v;
}

bool parse(const uint8_t* data, size_t size, Art* art) {
    *art = Art();
    if (!data || size < 24 || memcmp(data, kMagic, 4) != 0) return false;
    const uint32_t key = get32(data + 4);
    const uint32_t bw = get32(data + 8), bh = get32(data + 12), lw = get32(data + 16), lh = get32(data + 20);
    if (bw > 2048 || bh > 2048 || lw > 1024 || lh > 1024) return false;
    const uint64_t bn = (uint64_t)bw * bh, ln = (uint64_t)lw * lh;
    if ((bn == 0) != (bw == 0 || bh == 0) || (ln == 0) != (lw == 0 || lh == 0)) return false;
    if (24 + (bn + ln) * 4 != size) return false;
    if (bn + ln == 0) return false;
    art->key = key;
    if (bn) {
        art->bg.w = (int)bw;
        art->bg.h = (int)bh;
        art->bg.px.resize((size_t)bn);
        memcpy(art->bg.px.data(), data + 24, (size_t)bn * 4);
    }
    if (ln) {
        art->logo.w = (int)lw;
        art->logo.h = (int)lh;
        art->logo.px.resize((size_t)ln);
        memcpy(art->logo.px.data(), data + 24 + bn * 4, (size_t)ln * 4);
    }
    return true;
}

void compose(const Art& art, int dim_percent, Image* out, int* logo_bottom) {
    const int w = art.bg.empty() ? 960 : art.bg.w;
    const int h = art.bg.empty() ? 544 : art.bg.h;
    out->w = w;
    out->h = h;
    out->px.assign((size_t)w * (size_t)h, pack(0x14, 0x14, 0x20, 255));   // the plain boot colour where the disc has no background
    if (!art.bg.empty()) {
        for (int y = 0; y < h; y++) {
            // darker towards the bottom: the boot lines and the progress bar sit there. 100 % of dim at the top, 55 % of it at the bottom.
            const int pct = dim_percent * (100 - 45 * y / h) / 100;
            const uint32_t* src = &art.bg.px[(size_t)y * w];
            uint32_t* dst = &out->px[(size_t)y * w];
            for (int x = 0; x < w; x++) {
                const uint32_t p = src[x];
                dst[x] = pack(ch(p, 0) * pct / 100, ch(p, 1) * pct / 100, ch(p, 2) * pct / 100, 255);
            }
        }
    }
    int bottom = 0;
    if (!art.logo.empty()) {
        const int x0 = (w - art.logo.w) / 2, y0 = 20;
        for (int y = 0; y < art.logo.h && y0 + y < h; y++) {
            for (int x = 0; x < art.logo.w; x++) {
                const int dx = x0 + x;
                if (dx < 0 || dx >= w) continue;
                const uint32_t s = art.logo.px[(size_t)y * art.logo.w + x];
                const int a = ch(s, 3);
                if (a == 0) continue;
                uint32_t& d = out->px[(size_t)(y0 + y) * w + dx];
                d = pack((ch(s, 0) * a + ch(d, 0) * (255 - a)) / 255, (ch(s, 1) * a + ch(d, 1) * (255 - a)) / 255,
                         (ch(s, 2) * a + ch(d, 2) * (255 - a)) / 255, 255);
            }
        }
        bottom = y0 + art.logo.h;
    }
    if (logo_bottom) *logo_bottom = bottom;
}

}  // namespace app::boot_art
