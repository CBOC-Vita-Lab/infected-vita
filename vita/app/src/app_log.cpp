#include "app_log.h"

#include <stdio.h>
#include <string.h>

#include <iostream>
#include <streambuf>
#include <string>

#include "platform.h"

namespace app {
namespace {

// Collects text until a newline, then hands each complete line to the platform logger. The output is thread safe through the
// logger's own lock. The partial-line buffer is per stream AND per thread: with the GE thread the guest and the renderer log at
// the same time, and one shared buffer is a data race (a std::exception from a string that two threads grew and cleared). A
// small table, under a lock, finds the calling thread's slot by its thread id; a line written in pieces by one thread
// ("[hdtex] ..." << value << ...) stays whole.
constexpr int kLineSlots = 8;
plat::Mutex* g_slot_lock = nullptr;
int slot_of(int* ids, int thid) {
    for (int i = 0; i < kLineSlots; i++)
        if (ids[i] == thid) return i;
    for (int i = 0; i < kLineSlots; i++)
        if (ids[i] == 0) {
            ids[i] = thid;
            return i;
        }
    return kLineSlots - 1;   // more threads than slots: the last one is shared
}
struct SlotLock {
    SlotLock() { if (g_slot_lock) plat::mutex_lock(g_slot_lock); }
    ~SlotLock() { if (g_slot_lock) plat::mutex_unlock(g_slot_lock); }
};

class LineBuf : public std::streambuf {
public:
    explicit LineBuf(const char* tag) : tag_(tag) {}
    ~LineBuf() override { flush_partial(); }

protected:
    int_type overflow(int_type c) override {
        if (c == traits_type::eof()) return traits_type::not_eof(c);
        SlotLock lock;
        put((char)c);
        return c;
    }
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        SlotLock lock;
        for (std::streamsize i = 0; i < n; i++) put(s[i]);
        return n;
    }
    int sync() override { return 0; }

private:
    void put(char c) {
        std::string& line = line_[slot_of(ids_, plat::thread_current_id())];
        if (c == '\n') {
            emit(line);
        } else if (c != '\r') {
            line.push_back(c);
            if (line.size() >= 900) emit(line);
        }
    }
    void emit(std::string& line) {
        plat::logf("%s%s", tag_, line.c_str());
        line.clear();
    }
    void flush_partial() {
        for (std::string& line : line_)
            if (!line.empty()) emit(line);
    }
    const char* tag_;
    int ids_[kLineSlots] = {};
    std::string line_[kLineSlots];
};

LineBuf g_out("");
LineBuf g_err("");

// C stdio: a cookie stream whose write callback splits lines the same way.
struct Cookie {
    const char* tag;
    int ids[kLineSlots];
    char line[kLineSlots][1000];
    size_t len[kLineSlots];
};
Cookie g_out_cookie{"", {0}, {{0}}, {0}};
Cookie g_err_cookie{"", {0}, {{0}}, {0}};

void cookie_flush(Cookie* c, int slot) {
    if (c->len[slot] == 0) return;
    c->line[slot][c->len[slot]] = 0;
    plat::logf("%s%s", c->tag, c->line[slot]);
    c->len[slot] = 0;
}

int cookie_write(void* cookie, const char* buf, int n) {
    Cookie* c = (Cookie*)cookie;
    SlotLock lock;
    const int slot = slot_of(c->ids, plat::thread_current_id());
    for (int i = 0; i < n; i++) {
        char ch = buf[i];
        if (ch == '\n') {
            cookie_flush(c, slot);
        } else if (ch != '\r') {
            c->line[slot][c->len[slot]++] = ch;
            if (c->len[slot] >= sizeof(c->line[slot]) - 1) cookie_flush(c, slot);
        }
    }
    return n;
}

}  // namespace

void log_redirect_streams() {
    if (!g_slot_lock) g_slot_lock = plat::mutex_create("log-lines");
    std::cout.rdbuf(&g_out);
    std::cerr.rdbuf(&g_err);
    std::clog.rdbuf(&g_err);
    std::cout.setf(std::ios::unitbuf);
    std::cerr.setf(std::ios::unitbuf);
    FILE* out = funopen(&g_out_cookie, nullptr, cookie_write, nullptr, nullptr);
    FILE* err = funopen(&g_err_cookie, nullptr, cookie_write, nullptr, nullptr);
    if (out) {
        setvbuf(out, nullptr, _IOLBF, 1024);
        stdout = out;
    }
    if (err) {
        setvbuf(err, nullptr, _IONBF, 0);
        stderr = err;
    }
    plat::logf("log: stdout/stderr/iostreams routed to the platform logger (%s, %s)", out ? "stdout ok" : "stdout NOT redirected",
               err ? "stderr ok" : "stderr NOT redirected");
}

}  // namespace app
