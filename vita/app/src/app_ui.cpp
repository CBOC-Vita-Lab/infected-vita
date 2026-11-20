#include "app_ui.h"

#include <stdio.h>
#include <string.h>

#include <vitaGL.h>

#include "app_config.h"
#include "font8x8_basic.h"
#include "platform.h"

namespace app::ui {
namespace {

constexpr int kLines = 26;
constexpr int kCols = 58;       // 8 px glyphs at scale 2 -> 16 px per column, 960 / 16 = 60 columns minus the margin

plat::Mutex* g_mtx = nullptr;
char g_title[96] = "Infected";
char g_lines[kLines][kCols + 1];
int g_head = 0;
int g_count = 0;
float g_progress = -1.0f;
char g_progress_label[96] = "";
bool g_boot_visible = true;
bool g_hud_on = false;
char g_hud1[160] = "";
char g_hud2[160] = "";
bool g_msg_on = false;
char g_msg_title[96] = "";
char g_msg_body[1200] = "";
char g_msg_footer[160] = "";
unsigned g_version = 1;         // bumped by every change of what the overlay shows (the GPU-composed overlay re-uploads on a new value)
bool g_composite = true;
std::vector<uint32_t> g_art;     // the disc's own picture behind the boot screen (960x544), empty without
int g_art_w = 0, g_art_h = 0, g_art_text_top = 0;

unsigned overlay_version() { return (g_msg_on || g_boot_visible || g_hud_on) ? g_version : 0; }

void text_draw(uint32_t* fb, int pitch, int fbw, int fbh, int x0, int y0, const char* s, uint32_t col, int scale) {
    int x = x0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c >= 128) c = '?';
        const unsigned char* g = font8x8_basic[c];
        for (int ry = 0; ry < 8; ry++) {
            for (int rx = 0; rx < 8; rx++) {
                if (!(g[ry] & (1 << rx))) continue;
                for (int sy = 0; sy < scale; sy++) {
                    int py = y0 + ry * scale + sy;
                    if (py < 0 || py >= fbh) continue;
                    uint32_t* row = fb + (size_t)py * pitch;
                    for (int sx = 0; sx < scale; sx++) {
                        int px = x + rx * scale + sx;
                        if (px >= 0 && px < fbw) row[px] = col;
                    }
                }
            }
        }
        x += 8 * scale;
        if (x >= fbw) break;
    }
}

void fill(uint32_t* fb, int pitch, int fbw, int fbh, int x0, int y0, int w, int h, uint32_t col) {
    for (int y = y0; y < y0 + h && y < fbh; y++) {
        if (y < 0) continue;
        uint32_t* row = fb + (size_t)y * pitch;
        for (int x = x0; x < x0 + w && x < fbw; x++)
            if (x >= 0) row[x] = col;
    }
}

// Blends one colour over a rectangle: alpha 0..255.
void shade(uint32_t* fb, int pitch, int fbw, int fbh, int x0, int y0, int w, int h, uint32_t col, int alpha) {
    const int cr = col & 255, cg = (col >> 8) & 255, cb = (col >> 16) & 255;
    for (int y = y0 < 0 ? 0 : y0; y < y0 + h && y < fbh; y++) {
        uint32_t* row = fb + (size_t)y * pitch;
        for (int x = x0 < 0 ? 0 : x0; x < x0 + w && x < fbw; x++) {
            const uint32_t p = row[x];
            const int r = (cr * alpha + (int)(p & 255) * (255 - alpha)) / 255;
            const int g = (cg * alpha + (int)((p >> 8) & 255) * (255 - alpha)) / 255;
            const int b = (cb * alpha + (int)((p >> 16) & 255) * (255 - alpha)) / 255;
            row[x] = 0xFF000000u | (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16);
        }
    }
}

bool art_fits(int w, int h) { return !g_art.empty() && g_art_w == w && g_art_h == h; }

void art_blit(uint32_t* fb, int pitch, int w, int h) {
    for (int y = 0; y < h; y++) memcpy(fb + (size_t)y * pitch, &g_art[(size_t)y * w], (size_t)w * 4);
}

// Colours are ABGR8888 words (A in the top byte).
constexpr uint32_t kWhite = 0xFFFFFFFF, kGrey = 0xFFB0B0B0, kYellow = 0xFF40E0FF, kRed = 0xFF5050FF, kBg = 0xFF201414, kBar = 0xFF40C040;

void overlay(void* fbp, int w, int h, int pitch) {
    uint32_t* fb = (uint32_t*)fbp;
    if (!g_mtx) return;
    plat::mutex_lock(g_mtx);
    if (g_msg_on) {
        if (art_fits(w, h)) {
            art_blit(fb, pitch, w, h);
            shade(fb, pitch, w, h, 0, 0, w, h, 0xFF101060, 215);   // the picture shows through, the screen is still red
        } else {
            fill(fb, pitch, w, h, 0, 0, w, h, 0xFF101060);
        }
        text_draw(fb, pitch, w, h, 16, 16, g_msg_title, kWhite, 3);
        // wrap the body at 58 columns
        const char* p = g_msg_body;
        int y = 64;
        while (*p && y < h - 60) {
            char line[kCols + 1];
            int n = 0;
            while (p[n] && p[n] != '\n' && n < kCols) n++;
            int brk = n;
            if (p[n] && p[n] != '\n') {   // break at the last space if the line was cut
                for (int i = n; i > 0; i--)
                    if (p[i] == ' ') { brk = i; break; }
            }
            memcpy(line, p, brk);
            line[brk] = 0;
            text_draw(fb, pitch, w, h, 16, y, line, kWhite, 2);
            y += 20;
            p += brk;
            while (*p == ' ') p++;
            if (*p == '\n') p++;
        }
        text_draw(fb, pitch, w, h, 16, h - 36, g_msg_footer, kYellow, 2);
    } else if (g_boot_visible && art_fits(w, h)) {
        // On the disc's own picture: the title under the logo, the last lines on a dark panel, the bar at the bottom.
        art_blit(fb, pitch, w, h);
        const int top = g_art_text_top > 0 ? g_art_text_top + 8 : 12;
        text_draw(fb, pitch, w, h, 16, top, g_title, kWhite, 3);
        const int rows = (h - 64 - (top + 40)) / 16 > kLines ? kLines : (h - 64 - (top + 40)) / 16;
        const int shown = g_count < rows ? g_count : rows;
        shade(fb, pitch, w, h, 8, top + 36, w - 16, rows * 16 + 8, 0xFF000000, 150);
        int y = top + 40;
        for (int i = g_count - shown; i < g_count; i++) {
            int idx = ((g_head - g_count + i) % kLines + kLines) % kLines;
            text_draw(fb, pitch, w, h, 16, y, g_lines[idx], i == g_count - 1 ? kYellow : kGrey, 2);
            y += 16;
        }
        if (g_progress >= 0.0f) {
            const int bx = 16, bw = w - 32, bh = 20, by = h - 52;
            shade(fb, pitch, w, h, bx - 8, by - 34, bw + 16, bh + 44, 0xFF000000, 150);
            fill(fb, pitch, w, h, bx - 2, by - 2, bw + 4, bh + 4, kWhite);
            fill(fb, pitch, w, h, bx, by, bw, bh, 0xFF302020);
            float f = g_progress > 1.0f ? 1.0f : g_progress;
            fill(fb, pitch, w, h, bx, by, (int)(bw * f), bh, kBar);
            text_draw(fb, pitch, w, h, bx, by - 22, g_progress_label, kWhite, 2);
        }
    } else if (g_boot_visible) {
        fill(fb, pitch, w, h, 0, 0, w, h, kBg);
        text_draw(fb, pitch, w, h, 16, 12, g_title, kWhite, 3);
        int y = 56;
        for (int i = 0; i < g_count; i++) {
            int idx = ((g_head - g_count + i) % kLines + kLines) % kLines;
            text_draw(fb, pitch, w, h, 16, y, g_lines[idx], i == g_count - 1 ? kYellow : kGrey, 2);
            y += 16;
        }
        if (g_progress >= 0.0f) {
            const int bx = 16, bw = w - 32, bh = 20, by = h - 52;
            fill(fb, pitch, w, h, bx - 2, by - 2, bw + 4, bh + 4, kWhite);
            fill(fb, pitch, w, h, bx, by, bw, bh, 0xFF302020);
            float f = g_progress > 1.0f ? 1.0f : g_progress;
            fill(fb, pitch, w, h, bx, by, (int)(bw * f), bh, kBar);
            text_draw(fb, pitch, w, h, bx, by - 22, g_progress_label, kWhite, 2);
        }
    }
    if (g_hud_on && !g_msg_on && !g_boot_visible) {
        fill(fb, pitch, w, h, 0, 0, w, 36, 0xA0000000);
        text_draw(fb, pitch, w, h, 4, 2, g_hud1, kWhite, 2);
        text_draw(fb, pitch, w, h, 4, 18, g_hud2, kYellow, 2);
    }
    plat::mutex_unlock(g_mtx);
}

void line_add(const char* text) {
    if (!g_mtx) return;
    plat::mutex_lock(g_mtx);
    snprintf(g_lines[g_head], sizeof(g_lines[g_head]), "%.*s", kCols, text);
    g_head = (g_head + 1) % kLines;
    if (g_count < kLines) g_count++;
    g_version++;
    plat::mutex_unlock(g_mtx);
}

}  // namespace

void init() {
    if (!g_mtx) g_mtx = plat::mutex_create("ui");
    const char* mode = cfg_get("overlay");
    const bool composite = !(mode && strcmp(mode, "cpu") == 0);
    g_composite = composite;
    if (composite)
        plat::gl_set_overlay_composite(overlay, overlay_version);
    else
        plat::gl_set_overlay(overlay);
}

void boot_title(const char* title) {
    plat::mutex_lock(g_mtx);
    snprintf(g_title, sizeof(g_title), "%s", title);
    g_boot_visible = true;
    g_version++;
    plat::mutex_unlock(g_mtx);
}

void boot_line(const char* fmt, ...) {
    char buf[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    plat::logf("boot: %s", buf);
    line_add(buf);
    if (plat::gl_is_ready()) present();
}

void boot_progress(float fraction, const char* label) {
    if (!g_mtx) return;
    plat::mutex_lock(g_mtx);
    g_progress = fraction;
    snprintf(g_progress_label, sizeof(g_progress_label), "%s", label ? label : "");
    g_version++;
    plat::mutex_unlock(g_mtx);
}

void set_art(std::vector<uint32_t>&& pixels, int w, int h, int text_top) {
    if (!g_mtx) g_mtx = plat::mutex_create("ui");
    plat::mutex_lock(g_mtx);
    g_art = std::move(pixels);
    g_art_w = w;
    g_art_h = h;
    g_art_text_top = text_top;
    g_version++;
    plat::mutex_unlock(g_mtx);
    if (plat::gl_is_ready()) present();
}

void boot_hide() {
    if (!g_mtx) return;
    plat::mutex_lock(g_mtx);
    g_boot_visible = false;
    if (!g_msg_on) std::vector<uint32_t>().swap(g_art);   // the 2 MiB of heap come back for the game
    g_progress = -1.0f;
    g_version++;
    plat::mutex_unlock(g_mtx);
}

void hud_set(const char* line1, const char* line2) {
    if (!g_mtx) return;
    plat::mutex_lock(g_mtx);
    snprintf(g_hud1, sizeof(g_hud1), "%s", line1 ? line1 : "");
    snprintf(g_hud2, sizeof(g_hud2), "%s", line2 ? line2 : "");
    g_version++;
    plat::mutex_unlock(g_mtx);
}

void hud_enable(bool on) {
    if (g_hud_on != on) g_version++;
    g_hud_on = on;
}

void message_show(const char* title, const char* body, const char* footer) {
    if (!g_mtx) g_mtx = plat::mutex_create("ui");
    plat::mutex_lock(g_mtx);
    snprintf(g_msg_title, sizeof(g_msg_title), "%s", title);
    snprintf(g_msg_body, sizeof(g_msg_body), "%s", body);
    snprintf(g_msg_footer, sizeof(g_msg_footer), "%s", footer);
    g_msg_on = true;
    g_version++;
    plat::mutex_unlock(g_mtx);
}

void message_hide() {
    if (!g_mtx) return;
    plat::mutex_lock(g_mtx);
    g_msg_on = false;
    g_version++;
    plat::mutex_unlock(g_mtx);
}

void present() {
    if (!plat::gl_is_ready()) return;
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    plat::gl_swap();
}

}  // namespace app::ui
