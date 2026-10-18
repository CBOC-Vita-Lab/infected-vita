// app_ui.h - the start-up screen, the status HUD and the fatal-error screen: text on top of the finished frame, drawn
// into an RGBA buffer on the CPU and shown by a textured quad (or by vitaGL's display callback with overlay=cpu), so none of it needs a
// renderer or a shader.
#pragma once

#include <stdarg.h>
#include <stdint.h>

namespace app::ui {

// Installs the overlay. Call right after plat::gl_init succeeded (or instead of GL on a headless build: everything is then log-only).
// overlay=gpu (config.txt, the default): drawn by the GPU as a textured quad at every swap (plat::gl_set_overlay_composite; the default, and the only one
// Vita3K shows); overlay=cpu: CPU-drawn into the display buffer by vitaGL's display callback.
void init();

// The boot screen: a title, a scrolling list of lines (also written to the log) and one progress bar.
void boot_title(const char* title);
void boot_line(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void boot_progress(float fraction, const char* label);   // fraction < 0 hides the bar
void boot_hide();                                         // the boot text goes away (the game's frames are on screen)

// The status HUD (hud=1 in config.txt): two lines at the top left, text from the status line.
void hud_set(const char* line1, const char* line2);
void hud_enable(bool on);

// A full-screen message in red (fatal errors): title, wrapped body and a footer such as "Press START to exit".
void message_show(const char* title, const char* body, const char* footer);
void message_hide();

// Draws one frame of the current screen from the GL thread (clear + swap). Used while nothing else presents frames.
void present();

}  // namespace app::ui
