#include "app_iso.h"

#include <stdio.h>

#include <exception>
#include <filesystem>

#include "app_config.h"
#include "app_ui.h"
#include "platform.h"
#include "sha256_stream.h"

#include "kernel/iso_image.hpp"
#include "profile.hpp"
#include "psprecomp/sha256.hpp"

namespace app {

IsoCheck iso_check(const char* rel_path) {
    IsoCheck r;
    char full[256];
    plat::path_resolve(plat::Root::Data, rel_path, full, sizeof(full));
    r.size = (uint64_t)(plat::file_size_of(plat::Root::Data, rel_path) < 0 ? 0 : plat::file_size_of(plat::Root::Data, rel_path));
    if (!plat::file_exists(plat::Root::Data, rel_path)) {
        r.status = IsoStatus::Missing;
        r.message = "No disc image found.\nCopy your own Infected ISO (US version ULUS-10054, 1.02) into ux0:data/infected/ and start again.\nAny file name ending in .iso will do.";
        return r;
    }
    if (r.size < 64ull * 1024 * 1024) {
        r.status = IsoStatus::NotAnIso;
        char buf[200];
        snprintf(buf, sizeof(buf), "%s is only %llu bytes: not a complete UMD image.", full, (unsigned long long)r.size);
        r.message = buf;
        return r;
    }

    ui::boot_line("Disc image: %s (%llu MiB)", full, (unsigned long long)(r.size >> 20));

    // Structure and executable.
    try {
        portablekit::IsoImage iso{std::filesystem::path(full)};
        const auto entry = iso.find("PSP_GAME/SYSDIR/BOOT.BIN");
        if (!entry || entry->directory) {
            r.status = IsoStatus::NoExecutable;
            r.message = std::string(full) + " is an ISO but has no PSP_GAME/SYSDIR/BOOT.BIN: not a PSP game disc (or the wrong game).";
            return r;
        }
        r.boot_bin.resize(entry->size);
        ui::boot_line("Reading PSP_GAME/SYSDIR/BOOT.BIN (%u KiB)", (unsigned)(entry->size / 1024));
        const size_t got = iso.read((uint64_t)entry->lba * portablekit::IsoImage::kSectorSize, {r.boot_bin.data(), r.boot_bin.size()});
        if (got != r.boot_bin.size()) {
            r.status = IsoStatus::Unreadable;
            r.message = std::string("Short read of BOOT.BIN from ") + full + " (the file is truncated or the memory card failed).";
            r.boot_bin.clear();
            return r;
        }
    } catch (const std::exception& e) {
        r.status = IsoStatus::NotAnIso;
        r.message = std::string("Cannot open ") + full + " as a PSP disc image: " + e.what();
        return r;
    }

    r.boot_sha256 = psprecomp::sha256_bytes({r.boot_bin.data(), r.boot_bin.size()});
    const std::string want = portablekit::game().executable_sha256;
    plat::logf("iso: BOOT.BIN sha256 %s (expected %s)", r.boot_sha256.c_str(), want.c_str());
    if (r.boot_sha256 != want) {
        r.status = IsoStatus::WrongExecutable;
        r.message = "The executable on the disc is not the one this build was generated from (Infected ULUS-10054 v1.02).\n"
                    "SHA-256 " + r.boot_sha256 + "\nexpected " + want;
        r.boot_bin.clear();
        return r;
    }
    ui::boot_line("BOOT.BIN matches ULUS-10054 1.02 (sha256 %.16s...)", r.boot_sha256.c_str());

    if (cfg_bool("verify_iso", false)) {
        plat::File* f = plat::file_open(plat::Root::Data, rel_path, plat::FileMode::Read, 0);
        if (!f) {
            r.status = IsoStatus::Unreadable;
            r.message = std::string("Cannot open ") + full + " for the hash check.";
            return r;
        }
        Sha256 sha;
        std::vector<uint8_t> chunk(1 << 20);
        uint64_t done = 0;
        const uint64_t t0 = plat::now_us();
        for (;;) {
            size_t n = plat::file_read(f, chunk.data(), chunk.size());
            if (!n) break;
            sha.update(chunk.data(), n);
            done += n;
            if ((done >> 20) % 8 == 0) {
                char label[96];
                snprintf(label, sizeof(label), "Checking the disc image: %llu of %llu MiB", (unsigned long long)(done >> 20), (unsigned long long)(r.size >> 20));
                ui::boot_progress((float)((double)done / (double)r.size), label);
                ui::present();
            }
            if (n < chunk.size()) break;
        }
        plat::file_close(f);
        ui::boot_progress(-1.0f, "");
        r.image_sha256 = sha.hex_digest();
        plat::logf("iso: image sha256 %s (%llu bytes in %u s)", r.image_sha256.c_str(), (unsigned long long)done, (unsigned)((plat::now_us() - t0) / 1000000));
        const std::string expect = cfg_str("iso_sha256", "");
        if (!expect.empty() && expect != r.image_sha256) {
            r.status = IsoStatus::ImageHashMismatch;
            r.message = "The disc image hash does not match iso_sha256 in config.txt.\nSHA-256 " + r.image_sha256 + "\nexpected " + expect;
            r.boot_bin.clear();
            return r;
        }
        ui::boot_line("Disc image SHA-256 %.16s... %s", r.image_sha256.c_str(), expect.empty() ? "(no iso_sha256 to compare)" : "matches");
    }
    r.status = IsoStatus::Ok;
    return r;
}

}  // namespace app
