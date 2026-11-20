// Host test of the log queue (platform/log_queue.h): lines of concurrent writers stay whole and in each writer's own order, the
// order of the file is the order of the tickets, a full queue drops and counts instead of blocking, and the slots are reused
// across many laps. Build and run on any machine:
//   g++ -O2 -std=c++17 -pthread -I../platform t_log_queue_host.cpp -o t_log_queue_host && ./t_log_queue_host
#include "log_queue.h"

#include <stdio.h>
#include <string.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

static int g_failures = 0;
static void check(bool ok, const char* what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_failures++;
}

int main() {
    // single thread: order, wrap-around over many laps
    {
        plat::LogQueue q;
        check(q.init(8), "init");
        check(!q.init(8), "a second init is refused");
        char out[plat::LogQueue::kSlotBytes];
        size_t len = 0;
        bool order = true;
        int next = 0;
        for (int lap = 0; lap < 50; lap++) {
            for (int i = 0; i < 6; i++) {
                char line[32];
                int n = snprintf(line, sizeof(line), "line %d", lap * 6 + i);
                order = order && q.push(line, (size_t)n);
            }
            while (q.pop(out, &len)) {
                char want[32];
                int n = snprintf(want, sizeof(want), "line %d", next++);
                order = order && len == (size_t)n && memcmp(out, want, len) == 0;
            }
        }
        check(order && next == 300, "300 lines over 50 laps of an 8-slot queue come out in order");
        check(!q.pop(out, &len), "an empty queue pops nothing");
    }
    // a full queue drops, counts, and recovers
    {
        plat::LogQueue q;
        q.init(4);
        int accepted = 0;
        for (int i = 0; i < 10; i++) accepted += q.push("x", 1) ? 1 : 0;
        check(accepted == 4 && q.dropped() == 6 && q.take_dropped() == 6 && q.take_dropped() == 0, "a full queue takes 4, drops 6 and reports the drops once");
        char out[plat::LogQueue::kSlotBytes];
        size_t len;
        int popped = 0;
        while (q.pop(out, &len)) popped++;
        check(popped == 4 && q.push("y", 1), "after draining it takes lines again");
    }
    // a long line is cut at the slot size, not overrun
    {
        plat::LogQueue q;
        q.init(2);
        std::string big(5000, 'z');
        q.push(big.data(), big.size());
        char out[plat::LogQueue::kSlotBytes];
        size_t len = 0;
        check(q.pop(out, &len) && len == plat::LogQueue::kSlotBytes, "a 5000-byte line is cut to the slot size");
    }
    // concurrent writers: whole lines, per-writer order, nothing lost when the consumer keeps up
    {
        plat::LogQueue q;
        q.init(8192);
        const int kThreads = 6, kPer = 20000;
        std::atomic<bool> done{false};
        std::vector<int> last(kThreads, -1);
        long got = 0;
        bool whole = true, ordered = true;
        std::thread consumer([&] {
            char out[plat::LogQueue::kSlotBytes];
            size_t len;
            auto take = [&] {
                int t = -1, n = -1;
                char tail[8] = {0};
                // "T<t> <n> END"
                if (sscanf(std::string(out, len).c_str(), "T%d %d %7s", &t, &n, tail) != 3 || strcmp(tail, "END") != 0 || t < 0 || t >= kThreads) {
                    whole = false;
                    return;
                }
                ordered = ordered && n > last[t];
                last[t] = n;
                got++;
            };
            while (!done.load()) {
                if (q.pop(out, &len)) take();
            }
            while (q.pop(out, &len)) take();   // what was published before the producers finished
        });
        std::vector<std::thread> producers;
        std::atomic<long> accepted{0};
        for (int t = 0; t < kThreads; t++)
            producers.emplace_back([&, t] {
                for (int n = 0; n < kPer; n++) {
                    char line[64];
                    int len = snprintf(line, sizeof(line), "T%d %d END", t, n);
                    if (q.push(line, (size_t)len)) accepted++;
                }
            });
        for (auto& p : producers) p.join();
        done = true;
        consumer.join();
        check(whole, "every line that came out was whole");
        check(ordered, "each writer's lines came out in the order it wrote them");
        check(got == accepted.load() && (long)q.dropped() + accepted.load() == (long)kThreads * kPer, "every accepted line was taken once, and accepted + dropped is every push");
        printf("     %ld pushed accepted, %ld taken, %llu dropped\n", accepted.load(), got, (unsigned long long)q.dropped());
    }
    printf("%d failure(s)\n", g_failures);
    return g_failures ? 1 : 0;
}
