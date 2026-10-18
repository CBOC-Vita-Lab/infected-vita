#include "app_install.h"

#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <vector>

#include "platform.h"

namespace app {

namespace {

constexpr int64_t kMinDiscBytes = 64ll * 1024 * 1024;   // same floor as the disc check: a smaller file is not a UMD image

std::string lower(std::string s) {
    for (char& c : s) c = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
    return s;
}

bool ends_with(const std::string& s, const char* tail) {
    const size_t n = strlen(tail);
    return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}

void make_dirs(const char* path) {          // mkdir -p for "ux0:data/RetroFlow/COVERS/Homebrew"
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char* p = tmp; *p; p++) {
        if (*p == '/') {
            *p = 0;
            sceIoMkdir(tmp, 0777);
            *p = '/';
        }
    }
    sceIoMkdir(tmp, 0777);
}

bool exists(const char* path) {
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}

bool copy_file(const char* src, const char* dst) {
    SceUID in = sceIoOpen(src, SCE_O_RDONLY, 0);
    if (in < 0) return false;
    SceUID out = sceIoOpen(dst, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (out < 0) {
        sceIoClose(in);
        return false;
    }
    std::vector<uint8_t> buf(64 * 1024);
    bool ok = true;
    for (;;) {
        const int n = sceIoRead(in, buf.data(), (SceSize)buf.size());
        if (n < 0) { ok = false; break; }
        if (n == 0) break;
        if (sceIoWrite(out, buf.data(), (SceSize)n) != n) { ok = false; break; }
    }
    sceIoClose(in);
    sceIoClose(out);
    if (!ok) sceIoRemove(dst);              // never leave half a picture for RetroFlow to trip on
    return ok;
}

}  // namespace

std::string find_disc_name() {
    static const char* kDefault = "Infected.iso";
    if (plat::file_exists(plat::Root::Data, kDefault)) return kDefault;
    char dir[256];
    if (!plat::path_resolve(plat::Root::Data, "", dir, sizeof(dir))) return kDefault;
    const SceUID d = sceIoDopen(dir);
    if (d < 0) return kDefault;
    struct Candidate { std::string name; int rank; };
    std::vector<Candidate> found;
    SceIoDirent e;
    memset(&e, 0, sizeof(e));
    while (sceIoDread(d, &e) > 0) {
        const std::string name = e.d_name;
        const std::string low = lower(name);
        if (SCE_S_ISREG(e.d_stat.st_mode) && ends_with(low, ".iso") && (int64_t)e.d_stat.st_size >= kMinDiscBytes) {
            const bool named = low.find("infected") != std::string::npos || low.find("10054") != std::string::npos;
            found.push_back({name, named ? 0 : 1});
        }
        memset(&e, 0, sizeof(e));
    }
    sceIoDclose(d);
    if (found.empty()) return kDefault;
    std::sort(found.begin(), found.end(), [](const Candidate& a, const Candidate& b) {
        return a.rank != b.rank ? a.rank < b.rank : a.name < b.name;
    });
    plat::logf("disc image: %s is not there, using %s (%u .iso file%s of 64 MiB or more in the folder)", kDefault, found[0].name.c_str(),
               (unsigned)found.size(), found.size() == 1 ? "" : "s");
    return found[0].name;
}

void install_retroflow_art() {
    static const char* kTitleId = "INFC00001";      // the bubble's title id (VITA_TITLEID in CMakeLists.txt)
    static const char* kRoot = "ux0:data/RetroFlow";
    if (!exists(kRoot)) return;                     // RetroFlow is not installed (yet)
    struct Item { const char* packaged; const char* folder; };
    static const Item items[] = {{"retroflow/covers.png", "COVERS"}, {"retroflow/backgrounds.png", "BACKGROUNDS"}};
    for (const Item& it : items) {
        char dst_dir[256], dst[320], src[256];
        snprintf(dst_dir, sizeof(dst_dir), "%s/%s/Homebrew", kRoot, it.folder);
        snprintf(dst, sizeof(dst), "%s/%s.png", dst_dir, kTitleId);
        if (exists(dst)) continue;                  // the player's own picture, or ours from an earlier start
        if (!plat::path_resolve(plat::Root::App, it.packaged, src, sizeof(src)) || !exists(src)) continue;   // a package built without art
        make_dirs(dst_dir);
        plat::logf("RetroFlow: %s %s", copy_file(src, dst) ? "installed" : "could not write", dst);
    }
}

}  // namespace app
