// selftest/t_gl_stub.cpp - desktop twin of t_gl.cpp (the headless platform has no GL).
#include "st_common.h"

namespace st {
void gl_idle_frame() {}
void t6_gl_basics() { skip("T6", "gl-basics", "no GL in the %s build", plat::name()); }
void t7_render_to_texture() { skip("T7", "render-to-texture", "no GL in the %s build", plat::name()); }
void t8_draw_cost() { skip("T8", "draw-call-cost", "no GL in the %s build", plat::name()); }
}  // namespace st
