#pragma once

// The same code as host/file_replace.hpp of PortableKit (the settings and save writers there); this copy is for the platform layer,
// which does not include PortableKit (the PSVshell profile writer). Tested by selftest/t_file_replace_host.cpp.

// Replacing a file that already exists, where rename() may refuse to.
//
// A writer puts the new contents in `<target>.tmp` and renames that over the target, so an interrupted write never leaves a half
// file in place of a good one. POSIX rename() replaces the target in one step, but sceIoRename on the PS Vita (and rename() on
// Windows) refuses when the target exists, and the old fix (remove the target, then rename) leaves a window with no file at all.
//
// replace() therefore does:
//   1. rename(temp, target). Done when it works.
//   2. When it fails and the target exists: rename(target, target.bak), rename(temp, target), remove(target.bak).
//      A failure after the first of those puts the old file back.
// recover() is for the start of the next run: when the target is missing and `target.bak` is there (the run was killed between the
// two renames), the old file is put back, so the player has the last good save instead of none.
//
// The file system calls go through Ops so a test can make rename() refuse an existing target, or fail at a chosen step.

#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

namespace plat::filereplace {

struct Ops {
    bool (*rename)(const char *from, const char *to){nullptr};   // true when `from` is now `to`
    bool (*remove)(const char *path){nullptr};
    bool (*exists)(const char *path){nullptr};
};

inline bool default_rename(const char *from, const char *to) { return std::rename(from, to) == 0; }
inline bool default_remove(const char *path) { return std::remove(path) == 0; }
inline bool default_exists(const char *path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

inline const Ops &system_ops() {
    static const Ops ops{&default_rename, &default_remove, &default_exists};
    return ops;
}

// The ops in use: the system's unless a test installed its own (nullptr puts the system's back).
inline const Ops *&override_slot() {
    static const Ops *slot = nullptr;
    return slot;
}
inline void set_ops_for_test(const Ops *ops) { override_slot() = ops; }
inline const Ops &ops() { return override_slot() != nullptr ? *override_slot() : system_ops(); }

enum class Result {
    Renamed,            // the plain rename worked
    ReplacedViaBackup,  // the rename refused an existing target; the old file went to .bak, the new one in, the .bak was deleted
    Failed,             // the target is as it was; the temporary file is still there (the caller removes it)
};

inline Result replace(const Ops &o, const std::string &temp, const std::string &target) {
    if (o.rename(temp.c_str(), target.c_str())) return Result::Renamed;
    if (!o.exists(target.c_str())) return Result::Failed;   // not "the target is in the way": nothing more to try
    const std::string backup = target + ".bak";
    if (o.exists(backup.c_str())) o.remove(backup.c_str());   // a leftover of an earlier run
    if (!o.rename(target.c_str(), backup.c_str())) return Result::Failed;
    if (!o.rename(temp.c_str(), target.c_str())) {
        (void)o.rename(backup.c_str(), target.c_str());       // put the old one back
        return Result::Failed;
    }
    o.remove(backup.c_str());
    return Result::ReplacedViaBackup;
}
inline Result replace(const std::string &temp, const std::string &target) { return replace(ops(), temp, target); }

// True when the target was missing and `target.bak` has been put back in its place.
inline bool recover(const Ops &o, const std::string &target) {
    if (o.exists(target.c_str())) return false;
    const std::string backup = target + ".bak";
    if (!o.exists(backup.c_str())) return false;
    return o.rename(backup.c_str(), target.c_str());
}
inline bool recover(const std::string &target) { return recover(ops(), target); }

} // namespace plat::filereplace
