// gl_platform_vita.cpp - the Vita side of host/gpu/gl/gl_platform.h: the context is vitaGL (started by plat::gl_init, which the
// application calls early so the start-up screen can use it), the window is the Vita's screen and a frame ends with vglSwapBuffers.
//
// What vitaGL lacks (glp::Caps): no glBlendColor / CONSTANT_COLOR factors (GXM has no constant blend colour; the renderer builds
// those blends in the fragment shader instead), 4096 x 4096 maximum texture size.
#include "gpu/gl/gl_platform.h"

#include <string>

#include "app_config.h"
#include "platform.h"

namespace portablekit::gpu::glp {
namespace {
Caps g_caps;
bool g_created = false;
}  // namespace

Caps& caps() { return g_caps; }
bool has_window() { return true; }
void swap_buffers() { plat::gl_swap(); }

bool create_context(std::string& error) {
    if (g_created) return true;
    if (!plat::gl_is_ready()) {
        // Normally the application started vitaGL for its boot screen already; this is the fallback (bench modes).
        plat::GlParams p;
        p.ram_pool_mib = (int)app::cfg_int("gl_ram_pool_mib", 24);
        p.circular_pool_mib = (int)app::cfg_int("gl_circular_pool_mib", 16);
        p.rt_scenes_display = (int)app::cfg_int("gl_rt_scenes", 4);
        p.rt_scenes_fbo = (int)app::cfg_int("gl_rt_scenes_fbo", p.rt_scenes_display);
        p.shader_cache_dir = "shader_cache";
        if (!plat::gl_init(p)) {
            error = "plat::gl_init failed (see log.txt)";
            return false;
        }
    }
    const auto text = [](GLenum name) {
        const auto* value = reinterpret_cast<const char*>(glGetString(name));
        return value != nullptr ? std::string(value) : std::string();
    };
    g_caps.vendor = text(GL_VENDOR);
    g_caps.renderer = text(GL_RENDERER);
    g_caps.version = text(GL_VERSION);
    g_caps.blend_color = false;
    g_caps.blend_minmax = true;          // GXM has min/max blend functions and vitaGL maps glBlendEquation(GL_MIN/GL_MAX)
    g_caps.max_texture_size = 4096;
    g_caps.window_width = 960;
    g_caps.window_height = 544;
    g_caps.window_flip_y = true;         // vitaGL's display has GL's bottom-left origin; the targets keep PSP rows (row 0 = top at t = 0)
    g_created = true;
    plat::logf("GL: %s | %s | %s", g_caps.vendor.c_str(), g_caps.renderer.c_str(), g_caps.version.c_str());
    return true;
}

void destroy_context() {
    plat::gl_shutdown();
    g_created = false;
}

}  // namespace portablekit::gpu::glp
