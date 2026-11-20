#include "app_boot_art.h"

#include <stdlib.h>

#include <exception>
#include <filesystem>
#include <vector>

#include "app_config.h"
#include "app_ui.h"
#include "boot_art_core.h"
#include "platform.h"

#include "kernel/iso_image.hpp"

namespace app::boot_art {
namespace {

constexpr const char* kCache = "art/boot_art.bin";
constexpr size_t kMaxPng = 2u << 20;   // PIC1 is about 100-300 KiB on a PSP disc; anything over 2 MiB is not one

uint32_t g_shown_key = 0;
bool g_shown = false;

bool read_file(portablekit::IsoImage& iso, const char* path, std::vector<uint8_t>* out) {
    const auto entry = iso.find(path);
    if (!entry || entry->directory || entry->size == 0 || entry->size > kMaxPng) return false;
    out->resize(entry->size);
    const size_t got = iso.read((uint64_t)entry->lba * portablekit::IsoImage::kSectorSize, {out->data(), out->size()});
    if (got != out->size()) {
        out->clear();
        return false;
    }
    return true;
}

void show(const Art& art) {
    Image screen;
    int bottom = 0;
    compose(art, 55, &screen, &bottom);
    ui::set_art(std::move(screen.px), screen.w, screen.h, bottom);
    g_shown = true;
    g_shown_key = art.key;
}

}  // namespace

static void refresh(portablekit::IsoImage& iso);

void load_cached() {
    if (!cfg_bool("ui_boot_art", true)) {
        plat::logf("boot art: off (ui_boot_art=0)");
        return;
    }
    void* data = nullptr;
    size_t size = 0;
    if (!plat::file_read_all(plat::Root::Data, kCache, &data, &size)) return;   // the first start: the disc has to be read first
    Art art;
    const bool ok = parse(static_cast<const uint8_t*>(data), size, &art);
    free(data);
    if (!ok) {
        plat::logf("boot art: %s is not usable, it will be made again from the disc", kCache);
        return;
    }
    show(art);
    plat::logf("boot art: from %s (%dx%d, logo %dx%d)", kCache, art.bg.w, art.bg.h, art.logo.w, art.logo.h);
}

void refresh_from_disc(const char* iso_path) {
    if (!cfg_bool("ui_boot_art", true)) return;
    try {
        portablekit::IsoImage iso{std::filesystem::path(iso_path)};
        refresh(iso);
    } catch (const std::exception& e) {
        plat::logf("boot art: the disc image could not be read for its pictures (%s)", e.what());
    }
}

static void refresh(portablekit::IsoImage& iso) {
    std::vector<uint8_t> pic1, icon0;
    const bool has_pic = read_file(iso, "PSP_GAME/PIC1.PNG", &pic1);
    const bool has_icon = read_file(iso, "PSP_GAME/ICON0.PNG", &icon0);
    if (!has_pic && !has_icon) {
        plat::logf("boot art: the disc has no PSP_GAME/PIC1.PNG or ICON0.PNG; the boot screen stays text only");
        return;
    }
    const uint32_t key = crc32(icon0.data(), icon0.size(), crc32(pic1.data(), pic1.size()));
    if (g_shown && g_shown_key == key) {
        plat::logf("boot art: the cached pictures are this disc's");
        return;
    }
    Art art;
    const uint64_t t0 = plat::now_us();
    if (!build(pic1.data(), pic1.size(), icon0.data(), icon0.size(), 960, 544, &art)) {
        plat::logf("boot art: the pictures on the disc could not be decoded; the boot screen stays text only");
        return;
    }
    const std::vector<uint8_t> file = serialize(art);
    plat::dir_make(plat::Root::Data, "art");
    const bool saved = plat::file_write_all(plat::Root::Data, kCache, file.data(), file.size());
    show(art);
    plat::logf("boot art: made from the disc in %u ms (PIC1 %s, ICON0 %s), %s %s", (unsigned)((plat::now_us() - t0) / 1000), has_pic ? "yes" : "no",
               has_icon ? "yes" : "no", saved ? "cached in" : "could NOT be cached in", kCache);
}

}  // namespace app::boot_art
