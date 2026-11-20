// app_boot_art.h - the boot screen's pictures from the player's own disc (config.txt ui_boot_art, default 1).
//
// PSP_GAME/PIC1.PNG and PSP_GAME/ICON0.PNG are read from the disc image once, enlarged to 960x544 (boot_art_core.h) and kept in
// ux0:data/infected/art/boot_art.bin, so the next start shows them at once, before the disc image is even opened. They are drawn behind
// the start-up messages. Without the pictures (no disc image yet, a disc without those files) the screen stays text only.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace app::boot_art {

// Right after the screen exists: shows the pictures of the last start, when ui_boot_art is on and the cache is there.
void load_cached();
// With the path of a disc image whose BOOT.BIN was found to be the right one: when the pictures on it are not the cached ones, makes and caches
// the new ones and shows them. Never throws; logs one line.
void refresh_from_disc(const char* iso_path);

}  // namespace app::boot_art
