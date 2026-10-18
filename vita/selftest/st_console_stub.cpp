// selftest/st_console_stub.cpp - desktop twin: no display, the panel is only mirrored to the log.
#include "st_common.h"

namespace st {
bool console_open() { return false; }
void console_close() {}
void console_refresh() {}
}  // namespace st
