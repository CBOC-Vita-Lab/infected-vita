// log_queue.h - the queue between the threads that write log lines and the one thread that writes the file.
//
// Many producers, one consumer, fixed slots, no allocation after init, no lock: a producer takes a ticket (compare-and-swap on the
// tail), copies its line into the slot and publishes it; the consumer takes the slots in ticket order. So the lines of one thread
// stay in the order it wrote them, and the file shows the order the tickets were taken. A full queue drops the new line and
// counts it (the game thread never waits for the memory card).
// Header only and free of platform calls: the Vita platform layer (platform_vita.cpp) and the host test
// (selftest/t_log_queue_host.cpp) run the same code.
#pragma once

#include <atomic>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace plat {

class LogQueue {
public:
    static constexpr uint32_t kSlotBytes = 1024;   // a line is cut to this (logf's own limit is 1022 + newline)

    ~LogQueue() { free(slots_); }

    // slots: a power of two. false when the memory is not there.
    bool init(uint32_t slots) {
        if (slots_ || slots < 2 || (slots & (slots - 1))) return false;
        slots_ = (Slot*)calloc(slots, sizeof(Slot));
        if (!slots_) return false;
        mask_ = slots - 1;
        for (uint32_t i = 0; i < slots; i++) slots_[i].seq.store(i, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
        head_ = 0;
        return true;
    }
    bool ready() const { return slots_ != nullptr; }

    // Any thread. false: full, the line was dropped (and counted).
    bool push(const char* text, size_t len) {
        if (!slots_) return false;
        if (len > kSlotBytes) len = kSlotBytes;
        uint32_t pos = tail_.load(std::memory_order_relaxed);
        for (;;) {
            Slot& s = slots_[pos & mask_];
            const uint32_t seq = s.seq.load(std::memory_order_acquire);
            const int32_t diff = (int32_t)(seq - pos);
            if (diff == 0) {
                if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    memcpy(s.data, text, len);
                    s.len = (uint16_t)len;
                    s.seq.store(pos + 1, std::memory_order_release);
                    pushed_.fetch_add(1, std::memory_order_relaxed);
                    return true;
                }
            } else if (diff < 0) {
                dropped_.fetch_add(1, std::memory_order_relaxed);
                return false;   // the slot still holds a line the consumer has not taken: full
            } else {
                pos = tail_.load(std::memory_order_relaxed);
            }
        }
    }

    // The one consumer (the caller makes sure only one thread is in here). false: nothing published at the head.
    // The text is copied into out (capacity kSlotBytes) and its length is returned in len.
    bool pop(char* out, size_t* len) {
        if (!slots_) return false;
        Slot& s = slots_[head_ & mask_];
        if (s.seq.load(std::memory_order_acquire) != head_ + 1) return false;
        *len = s.len;
        memcpy(out, s.data, s.len);
        s.seq.store(head_ + mask_ + 1, std::memory_order_release);
        head_++;
        return true;
    }

    // Lines waiting (approximate: claimed but not yet published slots count).
    uint32_t depth() const { return slots_ ? tail_.load(std::memory_order_relaxed) - head_snapshot() : 0; }
    uint64_t pushed() const { return pushed_.load(std::memory_order_relaxed); }
    uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
    // Dropped lines since the last call (the writer reports them in the log).
    uint64_t take_dropped() {
        const uint64_t d = dropped_.load(std::memory_order_relaxed);
        const uint64_t r = d - reported_;
        reported_ = d;
        return r;
    }

private:
    struct Slot {
        std::atomic<uint32_t> seq;
        uint16_t len;
        char data[kSlotBytes];
    };
    uint32_t head_snapshot() const { return head_; }
    Slot* slots_ = nullptr;
    uint32_t mask_ = 0;
    alignas(64) std::atomic<uint32_t> tail_{0};
    alignas(64) uint32_t head_ = 0;   // the consumer's
    std::atomic<uint64_t> pushed_{0}, dropped_{0};
    uint64_t reported_ = 0;           // the consumer's
};

}  // namespace plat
