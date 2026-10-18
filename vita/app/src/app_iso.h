// app_iso.h - the game data check at start-up: the disc image in ux0:data/infected/ (name from app_install.h find_disc_name) must exist, be a PSP disc image and hold the
// executable (PSP_GAME/SYSDIR/BOOT.BIN, already a plain ELF) the recompiled code was generated from.
//
//   always (cheap):  file present, ISO9660 header, BOOT.BIN found and its SHA-256 equals the profile's executable_sha256
//   verify_iso=1:    additionally the SHA-256 of the whole image is computed (590 MB, a minute or two on the memory card), logged and,
//                    when iso_sha256=<hex> is set in config.txt, compared with it
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

namespace app {

enum class IsoStatus { Ok, Missing, Unreadable, NotAnIso, NoExecutable, WrongExecutable, ImageHashMismatch };

struct IsoCheck {
    IsoStatus status = IsoStatus::Missing;
    std::string message;                 // one or two lines for the screen, ending without a newline
    uint64_t size = 0;
    std::string boot_sha256;
    std::string image_sha256;            // only with verify_iso=1
    std::vector<uint8_t> boot_bin;       // the executable, when status == Ok
};

// rel_path is relative to the data root. Draws progress on the boot screen for the long steps.
IsoCheck iso_check(const char* rel_path);

}  // namespace app
