// Host test of platform/retry.h and platform/file_replace.h: the core-3 probe retry with injected failures, the throttle of the
// power tick, the late-period counter of the core-3 watchdog, and the file replace when a rename refuses an existing target.
//   g++ -O2 -std=c++17 -I../platform t_retry_host.cpp -o t_retry_host && ./t_retry_host
#include "audio_rate.h"
#include "retry.h"
#include "file_replace.h"

#include <stdio.h>
#include <stdlib.h>

#include <map>
#include <set>
#include <string>

static int g_failures = 0;
static void check(bool ok, const char* what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_failures++;
}

// An in-memory file system whose rename refuses an existing target, and can be told to die after N renames.
static std::map<std::string, std::string> g_fs;
static int g_renames_left = -1;
static bool fs_exists(const char* p) { return g_fs.count(p) != 0; }
static bool fs_remove(const char* p) { return g_fs.erase(p) != 0; }
static bool fs_rename(const char* a, const char* b) {
    if (g_renames_left == 0) return false;
    if (g_renames_left > 0) g_renames_left--;
    if (g_fs.count(b) || !g_fs.count(a)) return false;
    g_fs[b] = g_fs[a];
    g_fs.erase(a);
    return true;
}
static const plat::filereplace::Ops kOps{&fs_rename, &fs_remove, &fs_exists};

int main() {
    // probe retry
    {
        int calls = 0, sleeps = 0, tried = 0;
        int r = plat::retry_probe(6, [&](int) { calls++; return calls == 4; }, [&](int) { sleeps++; }, &tried);
        check(r == 4 && calls == 4 && sleeps == 3 && tried == 4, "a probe that fails three times and then works: success on try 4, three pauses");
        calls = sleeps = 0;
        r = plat::retry_probe(6, [&](int) { calls++; return false; }, [&](int) { sleeps++; }, &tried);
        check(r == 0 && calls == 6 && sleeps == 5 && tried == 6, "a probe that never works: six tries, five pauses (none after the last)");
        calls = sleeps = 0;
        r = plat::retry_probe(6, [&](int) { calls++; return true; }, [&](int) { sleeps++; });
        check(r == 1 && calls == 1 && sleeps == 0, "a probe that works at once: one try, no pause");
        r = plat::retry_probe(1, [&](int) { return false; }, [&](int) { sleeps++; });
        check(r == 0 && sleeps == 0, "attempts=1 (retry switched off) is a single try");
    }
    // throttle
    {
        plat::Throttle t;
        check(t.due(5000000, 1000000), "the first call is due");
        check(!t.due(5500000, 1000000) && !t.due(5999999, 1000000), "not due inside the period");
        check(t.due(6000000, 1000000) && !t.due(6100000, 1000000), "due again after the period");
    }
    // watchdog
    {
        check(!plat::period_is_late(21000, 21333, 3, 1000000) && plat::period_is_late(90000, 21333, 3, 1000000), "a period under 3x is on time, 90 ms is late");
        check(!plat::period_is_late(30000000, 21333, 3, 1000000), "a 30 s gap is a sleep, not a late thread");
        plat::LateCounter c;
        bool demote = c.note(true, 3);
        demote = c.note(true, 3) || demote;
        check(!demote, "two late periods in a row do not demote");
        check(c.note(false, 3) == false && c.in_a_row == 0, "an on-time period resets the run");
        c.note(true, 3); c.note(true, 3);
        check(c.note(true, 3) && c.total == 5, "three in a row demote");
    }
    // file replace with a refusing rename
    {
        using namespace plat::filereplace;
        g_fs.clear();
        g_fs["a"] = "old";
        g_fs["a.tmp"] = "new";
        check(replace(kOps, "a.tmp", "a") == Result::ReplacedViaBackup && g_fs["a"] == "new" && !g_fs.count("a.tmp") && !g_fs.count("a.bak"), "a refusing rename: replaced through .bak, nothing left over");
        g_fs.erase("a");
        g_fs["a.tmp"] = "first";
        check(replace(kOps, "a.tmp", "a") == Result::Renamed && g_fs["a"] == "first", "no existing target: the plain rename");
        // died after the old file went to .bak (renames: plain refused, target->.bak ok, then none)
        g_fs["a.tmp"] = "newer";
        g_renames_left = 2;
        check(replace(kOps, "a.tmp", "a") == Result::Failed, "a failure after the .bak step reports failure");
        g_renames_left = -1;
        check(!g_fs.count("a") && g_fs["a.bak"] == "first", "... and leaves the old file as .bak (killed run)");
        check(recover(kOps, "a") && g_fs["a"] == "first" && !g_fs.count("a.bak"), "recover() puts the .bak back");
        check(!recover(kOps, "a"), "recover() does nothing when the target is there");
        // failure of the second rename with a working put-back
        g_fs["a.tmp"] = "newest";
        g_fs.erase("a.tmp");   // the temp file is gone: rename(temp, target) fails, the old file is put back
        check(replace(kOps, "a.tmp", "a") == Result::Failed && g_fs["a"] == "first" && !g_fs.count("a.bak"), "a missing temp file: the target is untouched");
    }
    // the clock follower settles at the target it is given (the sink's cushion): a queue held at 5000 frames is no error with target 5000
    {
        plat::RateControl a, b;
        b.target = 5000.0f;
        uint32_t sa = 0, sb = 0;
        for (int i = 0; i < 2000; i++) { sa = a.update(1000000, 5000); sb = b.update(1000000, 5000); }
        check(a.ppm > 1000.0f && b.ppm < 1.0f && b.ppm > -1.0f, "the follower pushes a 5000-frame queue down toward 3072, and leaves it alone with target 5000");
        (void)sa; (void)sb;
    }
    printf("%d failure(s)\n", g_failures);
    return g_failures ? 1 : 0;
}
