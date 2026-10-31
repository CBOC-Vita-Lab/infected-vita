// platform/vita/gl_vita.cpp - GL context via vitaGL (see /VITAGL_NOTES.md for the reasoning behind every setting).
#include "../platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/display.h>
#include <psp2/gxm.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/sysmem.h>

#include <vitaGL.h>
#include <vitashark.h>

namespace plat {
namespace {
bool g_gl_ready = false;
GlOverlayFn g_overlay = nullptr;
GlOverlayFn g_comp_draw = nullptr;
GlOverlayVersionFn g_comp_version = nullptr;
GLuint g_comp_tex = 0;
uint32_t* g_comp_buf = nullptr;
unsigned g_comp_last = 0;
int g_w = 960, g_h = 544;

// Paths vitaGL/vitaShaRK try, in this order (gxm.c start_shader_compiler / vitashark.c DEFAULT_SHACCCG_PATH).
const char* const kShaccCgPaths[2] = {"ur0:/data/libshacccg.suprx", "ur0:data/external/libshacccg.suprx"};

bool exists(const char* path) {
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return false;
    sceIoClose(fd);
    return true;
}

void display_cb(void* fb) {
    if (g_overlay) g_overlay(fb, g_w, g_h, ((g_w + 63) / 64) * 64);
}

// The GPU-composed overlay (see platform.h): texture upload when the content changed, then one blended quad with the fixed-function
// pipeline (no program, no libshacccg needed). Everything it touches is set explicitly; the caller forgets its cached state afterwards.
void composite_overlay() {
    const unsigned v = g_comp_version ? g_comp_version() : 0;
    if (!v || !g_comp_draw) return;
    if (!g_comp_tex) {
        g_comp_buf = (uint32_t*)calloc((size_t)g_w * g_h, 4);
        if (!g_comp_buf) return;
        glGenTextures(1, &g_comp_tex);
        glBindTexture(GL_TEXTURE_2D, g_comp_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_w, g_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, g_comp_buf);
        g_comp_last = 0;
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_comp_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (v != g_comp_last) {
        memset(g_comp_buf, 0, (size_t)g_w * g_h * 4);
        g_comp_draw(g_comp_buf, g_w, g_h, g_w);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, g_w, g_h, GL_RGBA, GL_UNSIGNED_BYTE, g_comp_buf);
        g_comp_last = v;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glViewport(0, 0, g_w, g_h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glDepthMask(GL_FALSE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_TEXTURE_2D);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    static const float pos[8] = {-1.f, -1.f, 1.f, -1.f, -1.f, 1.f, 1.f, 1.f};
    static const float uv[8] = {0.f, 1.f, 1.f, 1.f, 0.f, 0.f, 1.f, 0.f};   // buffer row 0 is the top of the screen
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, pos);
    glTexCoordPointer(2, GL_FLOAT, 0, uv);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisable(GL_TEXTURE_2D);
    glDepthMask(GL_TRUE);
}
}  // namespace

bool gl_shader_compiler_available() {
    for (const char* p : kShaccCgPaths)
        if (exists(p)) return true;
    return false;
}

bool gl_init(const GlParams& p) {
    if (g_gl_ready) return true;

    // 1. libshacccg.suprx: not fatal (a pre-shipped shader cache can still serve the shaders) but it must be loud.
    bool have_cc = gl_shader_compiler_available();
    if (!have_cc) {
        logf("GL: libshacccg.suprx NOT FOUND (looked at %s and %s). GLSL shaders can only be loaded from the shader cache; any shader that "
             "is not in the cache will fail to compile (GL_INVALID_OPERATION). Install the shader compiler module "
             "(the usual 'shader compiler installer' homebrew step) so that ur0:data/libshacccg.suprx exists.",
             kShaccCgPaths[0], kShaccCgPaths[1]);
    } else {
        logf("GL: libshacccg.suprx present");
    }

    // 2. vitaGL options that must be set before vglInit*
    if (p.shader_cache_dir) {
        char path[200];
        if (path_resolve(Root::Data, p.shader_cache_dir, path, sizeof(path))) {
            dir_make(Root::Data, p.shader_cache_dir);
            size_t n = strlen(path);
            while (n && path[n - 1] == '/') path[--n] = 0;
            vglSetShaderCachePath(path);   // vitaGL then reads/writes <path>/v3/{v,f}/<XX>/<H>.gxp, H = XXH3_64bits(raw GLSL) printed %016llX (zero padded), XX = H>>56 printed %02X
            logf("GL: shader cache root %s", path);
        }
    }
    if (p.vdm_ring_kib > 0) vglSetVDMBufferSize((uint32_t)p.vdm_ring_kib * 1024);
    if (p.vertex_ring_kib > 0) vglSetVertexBufferSize((uint32_t)p.vertex_ring_kib * 1024);
    if (p.fragment_ring_kib > 0) vglSetFragmentBufferSize((uint32_t)p.fragment_ring_kib * 1024);
    if (p.usse_ring_kib > 0) vglSetUSSEBufferSize((uint32_t)p.usse_ring_kib * 1024);
    if (p.param_buffer_mib > 0) vglSetParamBufferSize((uint32_t)p.param_buffer_mib * 1024 * 1024);
    vglUseCachedMem(p.use_cached_mem ? GL_TRUE : GL_FALSE);
    vglSetupScratchMemory(p.scratch_for_dynamic ? GL_TRUE : GL_FALSE, p.scratch_for_stream ? GL_TRUE : GL_FALSE);   // no-op unless libvitaGL_fast.a
    vglSetCircularPoolSize((uint32_t)p.circular_pool_mib * 1024 * 1024);
    vglSetDisplayBufferCount(p.display_buffers);
    if (p.gc_on_core2) vglSetupGarbageCollector(0x10000100 + 8, helper_core() == 3 ? 0x80000 : SCE_KERNEL_CPU_MASK_USER_2);   // a bit less urgent than default; on core 3 when the system lets us
    {
        // Scenes per frame of the render targets (see GlParams). Out-of-range values are clamped: 8 is sceGxm's maximum
        // (SCE_GXM_MAX_SCENES_PER_RENDERTARGET) and the library, built without error checks, does not validate.
        const auto clamp_scenes = [](int n) { return n < 0 ? 0 : (n > 8 ? 8 : n); };
        const int display_scenes = clamp_scenes(p.rt_scenes_display);
        const int fbo_scenes = clamp_scenes(p.rt_scenes_fbo);
        if (display_scenes > 0 || fbo_scenes > 0)
            vglSetupRenderTargetScenesNum((uint8_t)(display_scenes > 0 ? display_scenes : 1), (uint8_t)(fbo_scenes > 0 ? fbo_scenes : 1));
        logf("GL: render targets are created for %d scene(s) a frame (display) and %d (framebuffer objects)%s",
             display_scenes > 0 ? display_scenes : 1, fbo_scenes > 0 ? fbo_scenes : 1,
             display_scenes > 0 || fbo_scenes > 0 ? "" : " (vitaGL's default)");
    }
    vglSetupRuntimeShaderCompiler((shark_opt)p.shark_opt_level, 0, 0, 0);
    if (p.display_buffers < 3) vglUseTripleBuffering(GL_FALSE);
    if (g_overlay) vglSetDisplayCallback(display_cb);

    // 3. memory thresholds. vglInitWithCustomThreshold(legacy, w, h, ram_thr, cdram_thr, phycont_thr, cdlg_thr, msaa):
    //    pool = free_at_init - threshold (0 when threshold >= free). We translate "pool size wanted" into thresholds.
    SceKernelFreeMemorySizeInfo fm;
    memset(&fm, 0, sizeof(fm));
    fm.size = sizeof(fm);
    sceKernelGetFreeMemorySize(&fm);
    const int64_t MiB = 1024 * 1024;
    int ram_thr = fm.size_user > p.ram_pool_mib * MiB ? (int)(fm.size_user - p.ram_pool_mib * MiB) : fm.size_user;
    int cdram_thr = p.cdram_pool_mib >= 0 ? (fm.size_cdram > p.cdram_pool_mib * MiB ? (int)(fm.size_cdram - p.cdram_pool_mib * MiB) : fm.size_cdram)
                                          : (int)(p.cdram_keep_mib * MiB);
    int phy_thr = fm.size_phycont > p.phycont_pool_mib * MiB ? (int)(fm.size_phycont - p.phycont_pool_mib * MiB) : fm.size_phycont;
    int cdlg_thr = p.cdlg_pool_mib > 0 ? (int)(0x8C6000 - p.cdlg_pool_mib * MiB) : 0x8C6000;   // 0x8C6000 = SCE_KERNEL_MAX_MAIN_CDIALOG_MEM_SIZE (vitaGL-internal)
    if (cdlg_thr < 0) cdlg_thr = 0;

    SceGxmMultisampleMode ms = p.msaa >= 4 ? SCE_GXM_MULTISAMPLE_4X : (p.msaa >= 2 ? SCE_GXM_MULTISAMPLE_2X : SCE_GXM_MULTISAMPLE_NONE);
    logf("GL: init %dx%d msaa=%d vsync=%d bufs=%d | free before: user=%d KiB cdram=%d KiB phycont=%d KiB | thresholds ram=%d cdram=%d phy=%d cdlg=%d",
         p.width, p.height, p.msaa, (int)p.vsync, p.display_buffers, (int)(fm.size_user / 1024), (int)(fm.size_cdram / 1024),
         (int)(fm.size_phycont / 1024), ram_thr, cdram_thr, phy_thr, cdlg_thr);
    uint64_t t0 = now_us();
    GLboolean fallback = vglInitWithCustomThreshold(p.legacy_pool_bytes, p.width, p.height, ram_thr, cdram_thr, phy_thr, cdlg_thr, ms);
    uint64_t dt = now_us() - t0;
    // NOTE: the return value is "display size had to fall back to the maximum", not an error flag; vitaGL has no failure return.
    g_w = p.width; g_h = p.height;
    if (fallback) {
        int mw = 0, mh = 0;
        sceDisplayGetMaximumFrameBufResolution(&mw, &mh);
        g_w = mw < g_w ? mw : g_w; g_h = mh < g_h ? mh : g_h;
        logf("GL: requested size exceeds the display maximum; vitaGL fell back to %dx%d", g_w, g_h);
    }
    vglWaitVblankStart(p.vsync ? GL_TRUE : GL_FALSE);
    g_gl_ready = true;
    MemInfo mi;
    mem_info(&mi);
    logf("GL: vitaGL up in %u ms | pools: ram free %u/%u KiB, cdram free %u/%u KiB, phycont %u/%u KiB | system free after: user=%d KiB cdram=%d KiB",
         (unsigned)(dt / 1000), (unsigned)(vglMemFree(VGL_MEM_RAM) / 1024), (unsigned)(vglMemTotal(VGL_MEM_RAM) / 1024),
         (unsigned)(vglMemFree(VGL_MEM_VRAM) / 1024), (unsigned)(vglMemTotal(VGL_MEM_VRAM) / 1024),
         (unsigned)(vglMemFree(VGL_MEM_PHYCONT) / 1024), (unsigned)(vglMemTotal(VGL_MEM_PHYCONT) / 1024), (int)(mi.user_free / 1024),
         (int)(mi.cdram_free / 1024));
    return true;
}

bool gl_is_ready() { return g_gl_ready; }
void gl_swap() {
    if (g_comp_draw) composite_overlay();
    vglSwapBuffers(GL_FALSE);
}
void gl_set_vsync(bool on) { vglWaitVblankStart(on ? GL_TRUE : GL_FALSE); }
void gl_shutdown() {
    // vitaGL (9bf3e3f) has no vglEnd(); resources are reclaimed when the process exits.
    logf("GL: shutdown requested (vitaGL cannot be torn down; freed at process exit)");
    g_gl_ready = false;
}
void* gl_get_proc(const char* name) { return vglGetProcAddress(name); }
void gl_set_overlay_composite(GlOverlayFn draw, GlOverlayVersionFn version) {
    g_comp_draw = draw;
    g_comp_version = version;
}
void gl_set_overlay(GlOverlayFn fn) {
    g_overlay = fn;
    if (g_gl_ready) vglSetDisplayCallback(fn ? display_cb : nullptr);
}

}  // namespace plat
