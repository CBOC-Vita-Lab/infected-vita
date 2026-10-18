// selftest/t_gl.cpp - T6 GL basics, T7 render-to-texture, T8 draw-call cost. Vita only (vitaGL). Everything here runs on the GL
// worker thread (core 1), the one thread that called plat::gl_init (VITAGL_NOTES.md section 3: single GL thread).
//
// The shaders are GLSL ES 1.00, written in the style the port will use: one transform VS (mat4 uniform, 3 attributes, 2 varyings)
// with a textured/tinted/alpha-tested FS and a flat "fix colour" FS. They are compiled with a per-run salt so "cold" (shader cache
// miss: translation + libshacccg compile) and "warm" (cache hit) are measured every run regardless of what is on disk.
#include "st_common.h"

#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vitaGL.h>

namespace st {

namespace {

// ------------------------------------------------------------------------------------------------ shared GL state
bool g_gl_tried = false, g_gl_ok = false;
constexpr int W = 960, H = 544;

struct Vtx {                 // 24 bytes, interleaved: the layout the port's vertex converter will produce
    float x, y, z;
    float u, v;
    uint8_t r, g, b, a;
};
static_assert(sizeof(Vtx) == 24, "Vtx");

const char* kVS =
    "uniform mat4 u_mvp;\n"
    "uniform vec4 u_uvx;\n"
    "attribute vec3 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "attribute vec4 a_col;\n"
    "varying vec2 v_uv;\n"
    "varying vec4 v_col;\n"
    "void main() {\n"
    "    gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
    "    v_uv = a_uv * u_uvx.xy + u_uvx.zw;\n"
    "    v_col = a_col;\n"
    "}\n";

const char* kFS_tex =
    "precision mediump float;\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec4 u_tint;\n"
    "uniform float u_aref;\n"
    "varying vec2 v_uv;\n"
    "varying vec4 v_col;\n"
    "void main() {\n"
    "    vec4 c = texture2D(u_tex, v_uv) * v_col * u_tint;\n"
    "    if (c.a < u_aref) discard;\n"
    "    gl_FragColor = c;\n"
    "}\n";

const char* kFS_fix =
    "precision mediump float;\n"
    "uniform vec4 u_fix;\n"
    "varying vec2 v_uv;\n"
    "varying vec4 v_col;\n"
    "void main() {\n"
    "    gl_FragColor = v_col * u_fix;\n"
    "}\n";

// second textured FS used by T8 so that program switches really switch fragment programs (modulate x2 with a fog-like term)
const char* kFS_tex2 =
    "precision mediump float;\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec4 u_tint;\n"
    "uniform float u_aref;\n"
    "varying vec2 v_uv;\n"
    "varying vec4 v_col;\n"
    "void main() {\n"
    "    vec4 t = texture2D(u_tex, v_uv);\n"
    "    vec4 c = clamp(t * v_col * 2.0, 0.0, 1.0) * u_tint;\n"
    "    gl_FragColor = vec4(c.rgb, c.a * (u_aref + 1.0));\n"
    "}\n";

struct Prog {
    GLuint prog = 0, vs = 0, fs = 0;
    GLint u_mvp = -1, u_uvx = -1, u_tex = -1, u_tint = -1, u_aref = -1, u_fix = -1;
    double ms_compile = 0, ms_link = 0;
};

const float kIdent[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

void gl_errors(const char* test, const char* where) {
    GLenum e = glGetError();
    if (e != GL_NO_ERROR) {
        info(test, "glerror", "glGetError() = 0x%04X after %s", (unsigned)e, where);
    }
}

bool compile_shader(GLenum type, const char* body, uint64_t salt, GLuint* out, double* ms, char* err, size_t errn) {
    char* src = (char*)malloc(strlen(body) + 64);
    snprintf(src, strlen(body) + 64, "// st-salt %llu\n%s", (unsigned long long)salt, body);
    GLuint sh = glCreateShader(type);
    const char* p = src;
    uint64_t t0 = plat::now_us();
    glShaderSource(sh, 1, &p, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    *ms += (double)(plat::now_us() - t0) / 1000.0;
    if (!ok) {
        GLsizei n = 0;
        glGetShaderInfoLog(sh, (GLsizei)errn - 1, &n, err);
        err[n > 0 ? n : 0] = 0;
    }
    free(src);
    *out = sh;
    return ok != 0;
}

bool build_prog(Prog& pr, const char* vs, const char* fs, uint64_t salt, char* err, size_t errn) {
    pr = Prog();
    err[0] = 0;
    if (!compile_shader(GL_VERTEX_SHADER, vs, salt, &pr.vs, &pr.ms_compile, err, errn)) return false;
    if (!compile_shader(GL_FRAGMENT_SHADER, fs, salt, &pr.fs, &pr.ms_compile, err + strlen(err), errn - strlen(err))) return false;
    pr.prog = glCreateProgram();
    glAttachShader(pr.prog, pr.vs);
    glAttachShader(pr.prog, pr.fs);
    glBindAttribLocation(pr.prog, 0, "a_pos");
    glBindAttribLocation(pr.prog, 1, "a_uv");
    glBindAttribLocation(pr.prog, 2, "a_col");
    uint64_t t0 = plat::now_us();
    glLinkProgram(pr.prog);
    GLint ok = 0;
    glGetProgramiv(pr.prog, GL_LINK_STATUS, &ok);
    pr.ms_link = (double)(plat::now_us() - t0) / 1000.0;
    if (!ok) {
        GLsizei n = 0;
        glGetProgramInfoLog(pr.prog, (GLsizei)errn - 1, &n, err);
        err[n > 0 ? n : 0] = 0;
        return false;
    }
    pr.u_mvp = glGetUniformLocation(pr.prog, "u_mvp");
    pr.u_uvx = glGetUniformLocation(pr.prog, "u_uvx");
    pr.u_tex = glGetUniformLocation(pr.prog, "u_tex");
    pr.u_tint = glGetUniformLocation(pr.prog, "u_tint");
    pr.u_aref = glGetUniformLocation(pr.prog, "u_aref");
    pr.u_fix = glGetUniformLocation(pr.prog, "u_fix");
    return true;
}

void use_prog(const Prog& p, float aref = 0.0f) {
    glUseProgram(p.prog);
    const float uvx[4] = {1, 1, 0, 0};
    const float tint[4] = {1, 1, 1, 1};
    const float fix[4] = {1, 1, 1, 1};
    if (p.u_mvp != -1) glUniformMatrix4fv(p.u_mvp, 1, GL_FALSE, kIdent);
    if (p.u_uvx != -1) glUniform4fv(p.u_uvx, 1, uvx);
    if (p.u_tex != -1) glUniform1i(p.u_tex, 0);
    if (p.u_tint != -1) glUniform4fv(p.u_tint, 1, tint);
    if (p.u_aref != -1) glUniform1f(p.u_aref, aref);
    if (p.u_fix != -1) glUniform4fv(p.u_fix, 1, fix);
}

void set_attribs_client(const Vtx* v) {
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vtx), &v->x);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), &v->u);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vtx), &v->r);
}

void set_attribs_vbo(GLuint vbo, size_t base_offset) {
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vtx), (const void*)(base_offset + 0));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (const void*)(base_offset + 12));
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vtx), (const void*)(base_offset + 20));
}

void make_quad(Vtx* v, float x0, float y0, float x1, float y1, float z, uint32_t rgba, float u0 = 0, float v0 = 0, float u1 = 1, float v1 = 1) {
    uint8_t r = rgba & 0xFF, g = (rgba >> 8) & 0xFF, b = (rgba >> 16) & 0xFF, a = (rgba >> 24) & 0xFF;
    v[0] = {x0, y0, z, u0, v0, r, g, b, a};
    v[1] = {x1, y0, z, u1, v0, r, g, b, a};
    v[2] = {x0, y1, z, u0, v1, r, g, b, a};
    v[3] = {x1, y1, z, u1, v1, r, g, b, a};
}
constexpr uint32_t RGBA(uint32_t r, uint32_t g, uint32_t b, uint32_t a = 255) { return r | (g << 8) | (b << 16) | (a << 24); }

void draw_quad(float x0, float y0, float x1, float y1, float z, uint32_t rgba, float u0 = 0, float v0 = 0, float u1 = 1, float v1 = 1) {
    Vtx v[4];
    make_quad(v, x0, y0, x1, y1, z, rgba, u0, v0, u1, v1);
    set_attribs_client(v);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

// NDC -> pixel helpers (GL window coordinates: origin bottom-left)
int px_x(float ndc, int w = W) { return (int)((ndc * 0.5f + 0.5f) * (float)w); }
int px_y(float ndc, int h = H) { return (int)((ndc * 0.5f + 0.5f) * (float)h); }

struct Px { int r, g, b, a; };
Px read_px(int x, int y) {
    uint8_t p[4] = {0, 0, 0, 0};
    glFinish();
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
    return {p[0], p[1], p[2], p[3]};
}
bool near_px(const Px& p, int r, int g, int b, int tol = 6) {
    return abs(p.r - r) <= tol && abs(p.g - g) <= tol && abs(p.b - b) <= tol;
}
const char* fmt_px(const Px& p, char* buf, size_t n) {
    snprintf(buf, n, "(%d,%d,%d,%d)", p.r, p.g, p.b, p.a);
    return buf;
}

GLuint make_tex_rgba(int w, int h, const uint8_t* rgba, GLint filter = GL_NEAREST) {
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    return t;
}

void clear_all(float r, float g, float b, float a) {
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glStencilMask(0xFF);
    glClearColor(r, g, b, a);
    glClearDepthf(1.0f);
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

void reset_state() {
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LEQUAL);
    glViewport(0, 0, W, H);
}

// ------------------------------------------------------------------------------------------------ overlay + idle
void overlay_cb(void* fb, int w, int h, int pitch) { panel_draw_overlay((uint32_t*)fb, pitch, w, h, 10); }

bool ensure_gl(const char* test) {
    if (g_gl_ok) return true;
    if (g_gl_tried) {
        skip(test, "gl", "GL initialisation failed earlier in this run");
        return false;
    }
    g_gl_tried = true;
    console_close();   // GXM takes over the display
    plat::GlParams gp;
    gp.width = W;
    gp.height = H;
    gp.msaa = cfg_int("gl.msaa", 0);
    gp.vsync = cfg_int("gl.vsync", 1) != 0;
    gp.display_buffers = cfg_int("gl.buffers", 3);
    gp.ram_pool_mib = cfg_int("gl.ram_mib", 24);
    gp.cdram_pool_mib = cfg_int("gl.cdram_mib", -1);
    gp.cdram_keep_mib = cfg_int("gl.cdram_keep_mib", 0);
    gp.phycont_pool_mib = cfg_int("gl.phycont_mib", 0);
    gp.circular_pool_mib = cfg_int("gl.circular_mib", 16);
    gp.vdm_ring_kib = cfg_int("gl.vdm_kib", 0);
    gp.vertex_ring_kib = cfg_int("gl.vertex_kib", 0);
    gp.fragment_ring_kib = cfg_int("gl.fragment_kib", 0);
    gp.usse_ring_kib = cfg_int("gl.usse_kib", 0);
    gp.scratch_for_dynamic = cfg_int("gl.scratch_dynamic", 0) != 0;   // only libvitaGL_fast.a: see platform.h
    gp.scratch_for_stream = cfg_int("gl.scratch_stream", 1) != 0;
    gp.shader_cache_dir = "selftest_shader_cache";
    plat::gl_set_overlay(overlay_cb);
    uint64_t t0 = plat::now_us();
    g_gl_ok = plat::gl_init(gp);
    if (!g_gl_ok) {
        console_open();
        fail(test, "gl-init", "plat::gl_init failed after %llu ms (see log.txt)", (unsigned long long)((plat::now_us() - t0) / 1000));
        return false;
    }
    pass(test, "gl-init", "vitaGL up in %llu ms (960x544, msaa %d, vsync %d, %d display buffers, ram pool %d MiB, circular pool %d MiB); libshacccg %s",
         (unsigned long long)((plat::now_us() - t0) / 1000), gp.msaa, (int)gp.vsync, gp.display_buffers, gp.ram_pool_mib, gp.circular_pool_mib,
         plat::gl_shader_compiler_available() ? "present" : "MISSING");
    return true;
}

}  // namespace

// called by the worker thread between jobs so the overlay keeps updating
void gl_idle_frame() {
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.05f, 0.08f, 0.14f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    plat::gl_swap();
}

// ================================================================================================================ T6
namespace {

struct ProbeCase { const char* name; const char* fs_body; bool expect_ok; };

void glsl_probes() {
    // Each probe is a complete fragment shader; the VS is the standard one so the interface matches. Informational: the point is
    // to learn, before porting, which constructs the vitaGL GLSL->Cg translator + libshacccg accept. Shader failures never FAIL the run.
    static const ProbeCase cases[] = {
        {"compound-assign-vec (c *= v_col)", "precision mediump float; uniform sampler2D u_tex; varying vec2 v_uv; varying vec4 v_col;\n"
                                             "void main(){ vec4 c = texture2D(u_tex, v_uv); c *= v_col; gl_FragColor = c; }", true},
        {"matrix*vec / mat*mat", "precision mediump float; uniform mat4 u_m; varying vec2 v_uv; varying vec4 v_col;\n"
                                 "void main(){ mat4 m2 = u_m * u_m; gl_FragColor = m2 * v_col; }", true},
        {"mat *= mat (known unsupported: vglMul rewrite skips *=)", "precision mediump float; uniform mat4 u_m; varying vec2 v_uv; varying vec4 v_col;\n"
                                                                    "void main(){ mat4 m2 = u_m; m2 *= u_m; gl_FragColor = m2 * v_col; }", false},
        {"builtins mix/clamp/dot/normalize/pow/fract/mod", "precision mediump float; varying vec2 v_uv; varying vec4 v_col;\n"
                                                           "void main(){ vec3 n = normalize(v_col.rgb); float d = dot(n, vec3(0.0,1.0,0.0));\n"
                                                           " gl_FragColor = vec4(mix(n, vec3(d), 0.5), pow(clamp(v_col.a,0.0,1.0), 2.0) + fract(v_uv.x) + mod(v_uv.y, 0.5)); }", true},
        {"for loop, constant bound", "precision mediump float; varying vec2 v_uv; varying vec4 v_col;\n"
                                     "void main(){ vec4 a = vec4(0.0); for (int i = 0; i < 4; i++) { a += v_col * 0.25; } gl_FragColor = a; }", true},
        {"uniform array vec4[8] with constant index", "precision mediump float; uniform vec4 u_pal[8]; varying vec2 v_uv; varying vec4 v_col;\n"
                                                     "void main(){ gl_FragColor = u_pal[3] * v_col; }", true},
        {"uniform array with dynamic index", "precision mediump float; uniform vec4 u_pal[8]; varying vec2 v_uv; varying vec4 v_col;\n"
                                            "void main(){ int i = int(v_col.r * 7.0); gl_FragColor = u_pal[i]; }", true},
        {"gl_FragCoord", "precision mediump float; varying vec2 v_uv; varying vec4 v_col;\n"
                         "void main(){ gl_FragColor = vec4(gl_FragCoord.x / 960.0, gl_FragCoord.y / 544.0, 0.0, 1.0) * v_col; }", true},
        {"two samplers", "precision mediump float; uniform sampler2D u_tex; uniform sampler2D u_tex2; varying vec2 v_uv; varying vec4 v_col;\n"
                         "void main(){ gl_FragColor = texture2D(u_tex, v_uv) * texture2D(u_tex2, v_uv.yx) * v_col; }", true},
        {"function with out param", "precision mediump float; varying vec2 v_uv; varying vec4 v_col;\n"
                                    "void shade(in vec4 c, out vec4 o){ o = c * 0.5; }\n"
                                    "void main(){ vec4 o; shade(v_col, o); gl_FragColor = o; }", true},
        {"variable named 't' (macro clobber: translator #defines s,t,st,ts,ss,tt)", "precision mediump float; varying vec2 v_uv; varying vec4 v_col;\n"
                                                                                    "void main(){ float t = v_uv.x; gl_FragColor = v_col * t; }", false},
        {"variable named 'sampler'/'matrix' (translator renames)", "precision mediump float; varying vec2 v_uv; varying vec4 v_col;\n"
                                                                    "void main(){ float matrix = v_uv.x; gl_FragColor = v_col * matrix; }", false},
        {"highp/lowp qualifiers (stripped by translator)", "precision mediump float; varying lowp vec4 v_col; varying highp vec2 v_uv;\n"
                                                           "void main(){ lowp vec4 c = v_col; highp float f = v_uv.x; gl_FragColor = c * f; }", true},
        {"texture2DProj", "precision mediump float; uniform sampler2D u_tex; varying vec2 v_uv; varying vec4 v_col;\n"
                          "void main(){ gl_FragColor = texture2DProj(u_tex, vec3(v_uv, 1.0)) * v_col; }", true},
        {"derivatives dFdx (needs GL_OES_standard_derivatives in ES 1.00)", "#extension GL_OES_standard_derivatives : enable\nprecision mediump float; varying vec2 v_uv; varying vec4 v_col;\n"
                                                                            "void main(){ gl_FragColor = vec4(dFdx(v_uv.x), dFdy(v_uv.y), 0.0, 1.0) + v_col; }", true},
        {"const array / struct", "precision mediump float; varying vec2 v_uv; varying vec4 v_col;\n"
                                 "struct L { vec4 c; float k; };\nvoid main(){ L l; l.c = v_col; l.k = 0.5; gl_FragColor = l.c * l.k; }", true},
    };
    char err[256];
    int n_ok = 0, n_total = 0, n_unexpected = 0;
    for (const ProbeCase& pc : cases) {
        Prog p;
        double ms = 0;
        GLuint vs = 0, fs = 0;
        uint64_t salt = plat::now_us();
        bool ok = compile_shader(GL_VERTEX_SHADER, kVS, salt, &vs, &ms, err, sizeof(err)) &&
                  compile_shader(GL_FRAGMENT_SHADER, pc.fs_body, salt, &fs, &ms, err, sizeof(err));
        if (ok) {
            GLuint prog = glCreateProgram();
            glAttachShader(prog, vs);
            glAttachShader(prog, fs);
            glBindAttribLocation(prog, 0, "a_pos");
            glBindAttribLocation(prog, 1, "a_uv");
            glBindAttribLocation(prog, 2, "a_col");
            glLinkProgram(prog);
            GLint lk = 0;
            glGetProgramiv(prog, GL_LINK_STATUS, &lk);
            ok = lk != 0;
            if (!ok) snprintf(err, sizeof(err), "link failed");
            glDeleteProgram(prog);
        }
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        n_total++;
        if (ok) n_ok++;
        if (ok != pc.expect_ok) n_unexpected++;
        for (char* c = err; *c; c++) if (*c == '\n' || *c == '\r') *c = ' ';
        info("T6", "glsl-probe", "%-62s -> %s%s (%.0f ms)%s%.80s", pc.name, ok ? "compiles" : "REJECTED", ok == pc.expect_ok ? "" : "  [UNEXPECTED]", ms,
             ok ? "" : " : ", ok ? "" : err);
        glGetError();
        poll_abort("T6-probe");
    }
    info("T6", "glsl-probe-summary", "%d/%d probe shaders compile; %d differ from what the vitaGL source predicts (see VITAGL_NOTES.md section 5)", n_ok, n_total, n_unexpected);
}

void shader_varying_limit() {
    // How many varyings can one VS->FS interface carry? (vitaGL maps varyings to TEXCOORDn/COLORn; hardware limit is 10 texcoords + 2 colours)
    char err[256];
    int best = 0;
    for (int n = 2; n <= 12; n += 2) {
        char vs[2048], fs[2048];
        int vl = snprintf(vs, sizeof(vs), "attribute vec3 a_pos;\nattribute vec2 a_uv;\nattribute vec4 a_col;\n");
        int fl = snprintf(fs, sizeof(fs), "precision mediump float;\n");
        for (int i = 0; i < n; i++) {
            vl += snprintf(vs + vl, sizeof(vs) - vl, "varying vec4 v%d;\n", i);
            fl += snprintf(fs + fl, sizeof(fs) - fl, "varying vec4 v%d;\n", i);
        }
        vl += snprintf(vs + vl, sizeof(vs) - vl, "void main(){ gl_Position = vec4(a_pos, 1.0);\n");
        fl += snprintf(fs + fl, sizeof(fs) - fl, "void main(){ vec4 s0 = vec4(0.0);\n");
        for (int i = 0; i < n; i++) {
            vl += snprintf(vs + vl, sizeof(vs) - vl, " v%d = a_col * %d.0 + vec4(a_uv, 0.0, 0.0);\n", i, i + 1);
            fl += snprintf(fs + fl, sizeof(fs) - fl, " s0 += v%d;\n", i);
        }
        vl += snprintf(vs + vl, sizeof(vs) - vl, "}\n");
        fl += snprintf(fs + fl, sizeof(fs) - fl, " gl_FragColor = s0; }\n");
        GLuint a = 0, b = 0;
        double ms = 0;
        uint64_t salt = plat::now_us() + (uint64_t)n;
        bool ok = compile_shader(GL_VERTEX_SHADER, vs, salt, &a, &ms, err, sizeof(err)) && compile_shader(GL_FRAGMENT_SHADER, fs, salt, &b, &ms, err, sizeof(err));
        if (ok) {
            GLuint prog = glCreateProgram();
            glAttachShader(prog, a);
            glAttachShader(prog, b);
            glLinkProgram(prog);
            GLint lk = 0;
            glGetProgramiv(prog, GL_LINK_STATUS, &lk);
            ok = lk != 0;
            glDeleteProgram(prog);
        }
        if (a) glDeleteShader(a);
        if (b) glDeleteShader(b);
        glGetError();
        if (ok) best = n; else break;
    }
    info("T6", "varying-limit", "interfaces with up to %d vec4 varyings compile and link (tried 2,4,..,12)", best);
}

}  // namespace

void t6_gl_basics() {
    if (!ensure_gl("T6")) return;
    status("T6 GL basics");
    reset_state();
    glGetError();

    // ---- identification and limits
    {
        const char* v = (const char*)glGetString(GL_VERSION);
        const char* r = (const char*)glGetString(GL_RENDERER);
        const char* ve = (const char*)glGetString(GL_VENDOR);
        const char* ex = (const char*)glGetString(GL_EXTENSIONS);
        GLint maxtex = 0, stencil_bits = 0, depth_bits = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxtex);
        glGetIntegerv(GL_STENCIL_BITS, &stencil_bits);
        glGetIntegerv(GL_DEPTH_BITS, &depth_bits);
        info("T6", "gl-strings", "GL_VERSION '%s' GL_RENDERER '%s' GL_VENDOR '%s' max texture %d, depth bits %d, stencil bits %d", v ? v : "?", r ? r : "?", ve ? ve : "?", maxtex, depth_bits, stencil_bits);
        if (ex) info("T6", "gl-extensions", "%.400s", ex);
    }

    // ---- representative shader set: cold (salted, cache miss) then warm (same source again, cache hit)
    Prog tex, fix;
    {
        const uint64_t salt = plat::now_us();
        char err[512];
        Prog c_tex, c_fix;
        plat::MemInfo mi0, mi1;
        plat::mem_info(&mi0);
        bool ok1 = build_prog(c_tex, kVS, kFS_tex, salt, err, sizeof(err));
        char err2[512];
        bool ok2 = build_prog(c_fix, kVS, kFS_fix, salt, err2, sizeof(err2));
        plat::mem_info(&mi1);
        info("T6", "shader-compiler-memory", "around the first compile (loads libshacccg.suprx if it was not loaded at init): user free %lld -> %lld KiB, cdram free %lld -> %lld KiB, newlib used %lld -> %lld KiB",
             (long long)(mi0.user_free / 1024), (long long)(mi1.user_free / 1024), (long long)(mi0.cdram_free / 1024), (long long)(mi1.cdram_free / 1024),
             (long long)(mi0.malloc_used / 1024), (long long)(mi1.malloc_used / 1024));
        const bool no_cc = !plat::gl_shader_compiler_available();
        // without libshacccg (e.g. Vita3K) a build failure is a known limitation of the environment, not a failure of the port stack
        if (!(ok1 && ok2) && no_cc) info("T6", "shaders-cold", "compile+link FAILED as expected without libshacccg.suprx: tex '%.160s' | fix '%.160s'", ok1 ? "ok" : err, ok2 ? "ok" : err2);
        else
        check(ok1 && ok2, "T6", "shaders-cold", "compile+link %s: tex program compile %.1f ms link %.1f ms; fix program compile %.1f ms link %.1f ms (first use of these sources: shader cache MISS, libshacccg %s)%s%.200s",
              ok1 && ok2 ? "ok" : "FAILED", c_tex.ms_compile, c_tex.ms_link, c_fix.ms_compile, c_fix.ms_link, plat::gl_shader_compiler_available() ? "present" : "MISSING",
              ok1 ? "" : " | tex: ", ok1 ? "" : err);
        if (!ok2) info("T6", "shaders-cold", "fix program log: %.200s", err2);
        if (!ok1 || !ok2) {
            if (!plat::gl_shader_compiler_available())
                limit("T6", "shaders", "libshacccg.suprx is missing: runtime GLSL cannot compile; install it or pre-ship a shader cache. Remaining GL tests that need shaders are skipped.");
            return;
        }
        Prog w_tex, w_fix;
        bool ok3 = build_prog(w_tex, kVS, kFS_tex, salt, err, sizeof(err));
        bool ok4 = build_prog(w_fix, kVS, kFS_fix, salt, err2, sizeof(err2));
        check(ok3 && ok4, "T6", "shaders-warm", "same sources again (cache HIT): tex compile %.1f ms link %.1f ms; fix compile %.1f ms link %.1f ms; speed-up x%.1f on compile",
              w_tex.ms_compile, w_tex.ms_link, w_fix.ms_compile, w_fix.ms_link,
              (w_tex.ms_compile + w_fix.ms_compile) > 0.01 ? (c_tex.ms_compile + c_fix.ms_compile) / (w_tex.ms_compile + w_fix.ms_compile) : 0.0);
        tex = ok3 ? w_tex : c_tex;
        fix = ok4 ? w_fix : c_fix;
        info("T6", "uniform-locations", "glGetUniformLocation values (vitaGL returns -(pointer) cast to GLint unless built with STRICT_UNIFORMS_COMPLIANCE; only -1 means 'not found'): u_mvp 0x%08X u_tex 0x%08X u_tint 0x%08X u_aref 0x%08X; missing uniform -> %d",
             (unsigned)tex.u_mvp, (unsigned)tex.u_tex, (unsigned)tex.u_tint, (unsigned)tex.u_aref, (int)glGetUniformLocation(tex.prog, "u_does_not_exist"));
    }
    if (cfg_int("t6.probes", 1)) {
        glsl_probes();
        shader_varying_limit();
    }
    poll_abort("T6");

    // ---- white texture used for untextured draws
    uint8_t white[4 * 4 * 4];
    memset(white, 255, sizeof(white));
    GLuint tex_white = make_tex_rgba(4, 4, white);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_white);
    char b1[48], b2[48], b3[48], b4[48];

    // ---- strips from client arrays
    {
        reset_state();
        use_prog(tex);
        clear_all(0, 0, 0, 1);
        draw_quad(-0.5f, -0.5f, 0.5f, 0.5f, 0.0f, RGBA(255, 0, 0));
        Px c = read_px(W / 2, H / 2), o = read_px(10, 10);
        check(near_px(c, 255, 0, 0) && near_px(o, 0, 0, 0), "T6", "strip-client-arrays", "textured+tinted triangle strip from client arrays: centre %s (want red), corner %s (want black)", fmt_px(c, b1, sizeof b1), fmt_px(o, b2, sizeof b2));
        plat::gl_swap();
    }
    // ---- strips from a VBO
    {
        reset_state();
        use_prog(tex);
        clear_all(0, 0, 0, 1);
        Vtx v[8];
        make_quad(v, -0.9f, -0.5f, -0.1f, 0.5f, 0.0f, RGBA(0, 255, 0));
        make_quad(v + 4, 0.1f, -0.5f, 0.9f, 0.5f, 0.0f, RGBA(0, 0, 255));
        GLuint vbo = 0;
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
        set_attribs_vbo(vbo, 0);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDrawArrays(GL_TRIANGLE_STRIP, 4, 4);   // varying `first` with constant attribute offsets: the cheap path
        Px a = read_px(px_x(-0.5f), H / 2), bq = read_px(px_x(0.5f), H / 2), o = read_px(W / 2, H / 2);
        check(near_px(a, 0, 255, 0) && near_px(bq, 0, 0, 255) && near_px(o, 0, 0, 0), "T6", "strip-vbo", "two strips from one VBO via glDrawArrays(first=0,4): left %s (green) right %s (blue) middle %s (black)",
              fmt_px(a, b1, sizeof b1), fmt_px(bq, b2, sizeof b2), fmt_px(o, b3, sizeof b3));
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glDeleteBuffers(1, &vbo);
        plat::gl_swap();
    }
    // ---- indexed draw: client indices and an element array buffer
    {
        reset_state();
        use_prog(tex);
        clear_all(0, 0, 0, 1);
        Vtx v[8];
        make_quad(v, -0.9f, -0.5f, -0.1f, 0.5f, 0.0f, RGBA(255, 255, 0));
        make_quad(v + 4, 0.1f, -0.5f, 0.9f, 0.5f, 0.0f, RGBA(255, 0, 255));
        // strip-ordered quads converted to triangle lists
        const uint16_t idx[12] = {0, 1, 2, 2, 1, 3, 4, 5, 6, 6, 5, 7};
        set_attribs_client(v);
        glDrawElements(GL_TRIANGLES, 12, GL_UNSIGNED_SHORT, idx);
        Px a = read_px(px_x(-0.5f), H / 2), bq = read_px(px_x(0.5f), H / 2);
        bool ok_client = near_px(a, 255, 255, 0) && near_px(bq, 255, 0, 255);
        // element array buffer
        clear_all(0, 0, 0, 1);
        GLuint ebo = 0, vbo = 0;
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
        set_attribs_vbo(vbo, 0);
        glGenBuffers(1, &ebo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STATIC_DRAW);
        glDrawElements(GL_TRIANGLES, 12, GL_UNSIGNED_SHORT, (const void*)0);
        Px a2 = read_px(px_x(-0.5f), H / 2), b2p = read_px(px_x(0.5f), H / 2);
        bool ok_vbo = near_px(a2, 255, 255, 0) && near_px(b2p, 255, 0, 255);
        check(ok_client, "T6", "indexed-client", "glDrawElements(GL_TRIANGLES, u16 client indices): left %s (yellow) right %s (magenta)", fmt_px(a, b1, sizeof b1), fmt_px(bq, b2, sizeof b2));
        check(ok_vbo, "T6", "indexed-vbo", "glDrawElements with GL_ELEMENT_ARRAY_BUFFER: left %s right %s", fmt_px(a2, b3, sizeof b3), fmt_px(b2p, b4, sizeof b4));
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glDeleteBuffers(1, &ebo);
        glDeleteBuffers(1, &vbo);
        plat::gl_swap();
    }
    // ---- paletted textures (OES), compared against the same image uploaded as RGBA8
    {
        reset_state();
        use_prog(tex);
        clear_all(0, 0, 0, 1);
        // 8x8 image, four 4x4 blocks with indices 0,1 (bottom row of texture data) and 2,3
        uint8_t idx8[64];
        uint8_t ref[8 * 8 * 4];
        const uint8_t pal[4][4] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 255, 255}};
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                int i = (x / 4) + 2 * (y / 4);
                idx8[y * 8 + x] = (uint8_t)i;
                memcpy(ref + (y * 8 + x) * 4, pal[i], 4);
            }
        // GL_PALETTE8_RGBA8_OES payload: 256 RGBA palette entries followed by the 8-bit indices
        uint8_t blob8[256 * 4 + 64];
        memset(blob8, 0, sizeof(blob8));
        for (int i = 0; i < 4; i++) memcpy(blob8 + i * 4, pal[i], 4);
        memcpy(blob8 + 256 * 4, idx8, 64);
        GLuint tp8 = 0, tref = make_tex_rgba(8, 8, ref);
        glGenTextures(1, &tp8);
        glBindTexture(GL_TEXTURE_2D, tp8);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGetError();
        glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_PALETTE8_RGBA8_OES, 8, 8, 0, (GLsizei)sizeof(blob8), blob8);
        GLenum e8 = glGetError();
        // P4: 16-entry palette + 4-bit indices, high nibble first (OES layout): 8x8 -> 4 bytes per row
        uint8_t blob4[16 * 4 + 32];
        memset(blob4, 0, sizeof(blob4));
        for (int i = 0; i < 4; i++) memcpy(blob4 + i * 4, pal[i], 4);
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x += 2) {
                uint8_t hi = idx8[y * 8 + x], lo = idx8[y * 8 + x + 1];
                blob4[16 * 4 + y * 4 + x / 2] = (uint8_t)((hi << 4) | lo);
            }
        GLuint tp4 = 0;
        glGenTextures(1, &tp4);
        glBindTexture(GL_TEXTURE_2D, tp4);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_PALETTE4_RGBA8_OES, 8, 8, 0, (GLsizei)sizeof(blob4), blob4);
        GLenum e4 = glGetError();

        // three 320x? panels: reference | P8 | P4, each sampled at 4 quadrant centres
        const float x0[3] = {-0.95f, -0.3f, 0.35f};
        GLuint texs[3] = {tref, tp8, tp4};
        for (int i = 0; i < 3; i++) {
            glBindTexture(GL_TEXTURE_2D, texs[i]);
            draw_quad(x0[i], -0.5f, x0[i] + 0.6f, 0.5f, 0.0f, RGBA(255, 255, 255));
        }
        Px q[3][4];
        for (int i = 0; i < 3; i++) {
            float cx0 = x0[i] + 0.15f, cx1 = x0[i] + 0.45f;
            q[i][0] = read_px(px_x(cx0), px_y(-0.25f));   // bottom-left quadrant of the quad
            q[i][1] = read_px(px_x(cx1), px_y(-0.25f));
            q[i][2] = read_px(px_x(cx0), px_y(0.25f));
            q[i][3] = read_px(px_x(cx1), px_y(0.25f));
        }
        bool ref_ok = near_px(q[0][0], 255, 0, 0) && near_px(q[0][1], 0, 255, 0) && near_px(q[0][2], 0, 0, 255) && near_px(q[0][3], 255, 255, 255);
        bool ref_flipped = near_px(q[0][0], 0, 0, 255) && near_px(q[0][1], 255, 255, 255) && near_px(q[0][2], 255, 0, 0) && near_px(q[0][3], 0, 255, 0);
        info("T6", "tex-orientation", "RGBA8 reference quadrants (BL,BR,TL,TR): %s %s %s %s -> %s", fmt_px(q[0][0], b1, sizeof b1), fmt_px(q[0][1], b2, sizeof b2), fmt_px(q[0][2], b3, sizeof b3), fmt_px(q[0][3], b4, sizeof b4),
             ref_ok ? "GL orientation (first row = bottom), as expected" : (ref_flipped ? "V-FLIPPED relative to GL convention" : "unexpected"));
        auto same = [&](int a, int b2i) {
            for (int k = 0; k < 4; k++)
                if (!near_px(q[a][k], q[b2i][k].r, q[b2i][k].g, q[b2i][k].b, 6)) return false;
            return true;
        };
        check(e8 == GL_NO_ERROR && same(0, 1), "T6", "paletted-p8", "GL_PALETTE8_RGBA8_OES via glCompressedTexImage2D (palette+indices): glError 0x%04X, quadrants %s %s %s %s match the RGBA8 reference: %s",
              (unsigned)e8, fmt_px(q[1][0], b1, sizeof b1), fmt_px(q[1][1], b2, sizeof b2), fmt_px(q[1][2], b3, sizeof b3), fmt_px(q[1][3], b4, sizeof b4), same(0, 1) ? "yes" : "NO");
        check(e4 == GL_NO_ERROR && same(0, 2), "T6", "paletted-p4", "GL_PALETTE4_RGBA8_OES (high nibble first): glError 0x%04X, quadrants %s %s %s %s match the RGBA8 reference: %s",
              (unsigned)e4, fmt_px(q[2][0], b1, sizeof b1), fmt_px(q[2][1], b2, sizeof b2), fmt_px(q[2][2], b3, sizeof b3), fmt_px(q[2][3], b4, sizeof b4), same(0, 2) ? "yes" : "NO");
        glBindTexture(GL_TEXTURE_2D, tex_white);
        GLuint del[3] = {tref, tp8, tp4};
        glDeleteTextures(3, del);
        plat::gl_swap();
    }
    // ---- alpha blending, blend colour (absent) + two-pass emulation
    {
        reset_state();
        use_prog(tex);
        clear_all(0, 0, 0, 1);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        draw_quad(-0.5f, -0.5f, 0.5f, 0.5f, 0.0f, RGBA(255, 0, 0, 128));
        Px p = read_px(W / 2, H / 2);
        check(near_px(p, 128, 0, 0, 4), "T6", "blend-alpha", "GL_SRC_ALPHA / GL_ONE_MINUS_SRC_ALPHA, red at alpha 128 over black: %s (want ~(128,0,0))", fmt_px(p, b1, sizeof b1));

        void* bc = plat::gl_get_proc("glBlendColor");
        if (!bc) limit("T6", "blend-color", "glBlendColor / GL_CONSTANT_COLOR / GL_CONSTANT_ALPHA factors are NOT implemented by vitaGL; the port must emulate PSP 'fix' blend factors (see next line)");
        else info("T6", "blend-color", "vglGetProcAddress(glBlendColor) = %p", bc);

        // emulation: result = src*A + dst*B for constants A=B=0.5. pass 1 multiplies dst by B ((ZERO, SRC_COLOR) with the FS emitting B),
        // pass 2 adds src*A ((ONE, ONE) with the FS emitting src*A).
        glDisable(GL_BLEND);
        clear_all(0, 0, 0, 1);
        use_prog(tex);
        draw_quad(-1, -1, 1, 1, 0.0f, RGBA(128, 64, 0));          // dst
        use_prog(fix);
        const float half[4] = {0.5f, 0.5f, 0.5f, 0.5f};
        glUniform4fv(fix.u_fix, 1, half);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ZERO, GL_SRC_COLOR);
        draw_quad(-0.5f, -0.5f, 0.5f, 0.5f, 0.0f, RGBA(255, 255, 255));   // dst *= 0.5
        glBlendFunc(GL_ONE, GL_ONE);
        draw_quad(-0.5f, -0.5f, 0.5f, 0.5f, 0.0f, RGBA(0, 0, 255));       // += src * 0.5
        Px e = read_px(W / 2, H / 2), outside = read_px(10, 10);
        check(near_px(e, 64, 32, 127, 4) && near_px(outside, 128, 64, 0, 4), "T6", "blend-color-emulation", "two-pass emulation of constant-colour blend (A=B=0.5): inside %s (want ~(64,32,127)), outside untouched %s (want (128,64,0))",
              fmt_px(e, b1, sizeof b1), fmt_px(outside, b2, sizeof b2));
        glDisable(GL_BLEND);
        plat::gl_swap();
    }
    // ---- colour mask
    {
        reset_state();
        use_prog(tex);
        clear_all(0, 0, 0, 1);
        glColorMask(GL_TRUE, GL_FALSE, GL_FALSE, GL_TRUE);
        draw_quad(-0.5f, -0.5f, 0.5f, 0.5f, 0.0f, RGBA(255, 255, 255));
        Px p = read_px(W / 2, H / 2);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        check(near_px(p, 255, 0, 0, 4), "T6", "color-mask", "glColorMask(R only) white quad over black: %s (want (255,0,0))", fmt_px(p, b1, sizeof b1));
        // mask also applies to glClear
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glColorMask(GL_FALSE, GL_TRUE, GL_FALSE, GL_TRUE);
        glClearColor(1, 1, 1, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        Px c = read_px(10, 10);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        if (near_px(c, 0, 255, 0, 4)) pass("T6", "color-mask-clear", "glClear honours glColorMask: %s (want (0,255,0))", fmt_px(c, b1, sizeof b1));
        else limit("T6", "color-mask-clear", "glClear ignores glColorMask in vitaGL (got %s, want (0,255,0)); draw a masked quad instead of clearing", fmt_px(c, b1, sizeof b1));
        plat::gl_swap();
    }
    // ---- scissor (draw and clear)
    {
        reset_state();
        use_prog(tex);
        clear_all(0, 0, 0, 1);
        glEnable(GL_SCISSOR_TEST);
        glScissor(100, 100, 200, 200);
        draw_quad(-1, -1, 1, 1, 0.0f, RGBA(255, 255, 255));
        Px in = read_px(150, 150), out1 = read_px(50, 50), out2 = read_px(350, 350);
        bool ok_draw = near_px(in, 255, 255, 255) && near_px(out1, 0, 0, 0) && near_px(out2, 0, 0, 0);
        glClearColor(1, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        Px cin = read_px(250, 250), cout = read_px(50, 400);
        bool ok_clear = near_px(cin, 255, 0, 0) && near_px(cout, 0, 0, 0);
        glDisable(GL_SCISSOR_TEST);
        check(ok_draw, "T6", "scissor-draw", "scissored full-screen quad: inside %s (white), outside %s %s (black)", fmt_px(in, b1, sizeof b1), fmt_px(out1, b2, sizeof b2), fmt_px(out2, b3, sizeof b3));
        check(ok_clear, "T6", "scissor-clear", "scissored glClear: inside %s (red), outside %s (black)", fmt_px(cin, b1, sizeof b1), fmt_px(cout, b2, sizeof b2));
        plat::gl_swap();
    }
    // ---- discard
    {
        reset_state();
        use_prog(tex, 0.5f);
        clear_all(0, 0, 1, 1);
        draw_quad(-0.9f, -0.5f, -0.1f, 0.5f, 0.0f, RGBA(255, 0, 0, 64));    // alpha 0.25 < 0.5 -> discarded
        draw_quad(0.1f, -0.5f, 0.9f, 0.5f, 0.0f, RGBA(255, 0, 0, 192));     // alpha 0.75 -> drawn
        Px d = read_px(px_x(-0.5f), H / 2), k = read_px(px_x(0.5f), H / 2);
        check(near_px(d, 0, 0, 255, 4) && near_px(k, 255, 0, 0, 4),
              "T6", "discard", "alpha-test via discard (u_aref 0.5): alpha .25 quad left %s (want clear colour (0,0,255)), alpha .75 quad right %s (want ~(255,0,0))", fmt_px(d, b1, sizeof b1), fmt_px(k, b2, sizeof b2));
        plat::gl_swap();
    }
    // ---- depth test
    {
        reset_state();
        use_prog(tex);
        clear_all(0, 0, 0, 1);
        glEnable(GL_DEPTH_TEST);
        draw_quad(-0.5f, -0.5f, 0.5f, 0.5f, -0.5f, RGBA(255, 0, 0));    // near, drawn first
        draw_quad(-0.25f, -0.25f, 0.75f, 0.75f, 0.5f, RGBA(0, 255, 0)); // far, must lose where they overlap
        Px ov = read_px(px_x(0.0f), px_y(0.0f)), only_far = read_px(px_x(0.6f), px_y(0.6f));
        check(near_px(ov, 255, 0, 0) && near_px(only_far, 0, 255, 0), "T6", "depth", "depth test LEQUAL: overlap %s (red, near wins), far-only %s (green)", fmt_px(ov, b1, sizeof b1), fmt_px(only_far, b2, sizeof b2));
        plat::gl_swap();
    }
    // ---- stencil
    {
        GLint sbits = 0;
        glGetIntegerv(GL_STENCIL_BITS, &sbits);
        if (sbits == 0) {
            limit("T6", "stencil", "default framebuffer has 0 stencil bits");
        } else {
            reset_state();
            use_prog(tex);
            clear_all(0, 0, 0, 1);
            glEnable(GL_STENCIL_TEST);
            glStencilFunc(GL_ALWAYS, 1, 0xFF);
            glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            draw_quad(-0.5f, -0.5f, 0.5f, 0.5f, 0.0f, RGBA(255, 255, 255));       // stencil only
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glStencilFunc(GL_EQUAL, 1, 0xFF);
            glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
            draw_quad(-1, -1, 1, 1, 0.0f, RGBA(0, 255, 255));                     // only where stencil == 1
            Px in = read_px(W / 2, H / 2), out = read_px(20, 20);
            glDisable(GL_STENCIL_TEST);
            check(near_px(in, 0, 255, 255) && near_px(out, 0, 0, 0), "T6", "stencil", "stencil write (REPLACE) then EQUAL test: inside %s (cyan), outside %s (black), stencil bits %d", fmt_px(in, b1, sizeof b1), fmt_px(out, b2, sizeof b2), (int)sbits);
            plat::gl_swap();
        }
    }
    // ---- stencil/depth/scissor survive a mid-frame scene break (readback or FBO switch)? Informational: decides STORE_DEPTH_STENCIL.
    {
        reset_state();
        use_prog(tex);
        clear_all(0, 0, 0, 1);
        glEnable(GL_DEPTH_TEST);
        draw_quad(-0.5f, -0.5f, 0.5f, 0.5f, -0.5f, RGBA(255, 0, 0));             // near quad, depth written
        (void)read_px(5, 5);                                                      // forces a scene break (sceGxmEndScene + finish)
        draw_quad(-0.25f, -0.25f, 0.75f, 0.75f, 0.5f, RGBA(0, 255, 0));          // far quad: must still lose in the overlap if depth was kept
        Px ov = read_px(px_x(0.0f), px_y(0.0f));
        if (near_px(ov, 255, 0, 0)) pass("T6", "depth-across-scene-break", "depth buffer content survived a mid-frame scene break (overlap %s is red)", fmt_px(ov, b1, sizeof b1));
        else limit("T6", "depth-across-scene-break", "depth buffer content did NOT survive a mid-frame scene break (overlap %s is not red): this library variant lacks STORE_DEPTH_STENCIL=1; use libvitaGL_store.a if the port interleaves FBO passes with depth-tested default-FB drawing", fmt_px(ov, b1, sizeof b1));
        plat::gl_swap();
    }
    glDeleteTextures(1, &tex_white);
    gl_errors("T6", "all T6 checks");
}

// ================================================================================================================ T7
void t7_render_to_texture() {
    if (!ensure_gl("T7")) return;
    status("T7 render to texture");
    reset_state();
    glGetError();
    char err[512];
    Prog tex;
    if (!build_prog(tex, kVS, kFS_tex, 12345, err, sizeof(err))) {
        if (!plat::gl_shader_compiler_available())
            limit("T7", "shader", "libshacccg.suprx is missing (Vita3K has no shader compiler either): no GLSL can be built, T7 skipped. %.120s", err);
        else
            fail("T7", "shader", "cannot build the textured program: %.200s", err);
        return;
    }
    use_prog(tex);
    char b1[48], b2[48], b3[48], b4[48];

    struct Variant { const char* name; GLenum ds_format; GLenum ds_attach; };
    const Variant variants[] = {
        {"fbo-depth16", GL_DEPTH_COMPONENT16, GL_DEPTH_ATTACHMENT},
        {"fbo-depth24-stencil8", GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL_ATTACHMENT},
    };
    uint8_t white[4 * 4 * 4];
    memset(white, 255, sizeof(white));
    GLuint tex_white = make_tex_rgba(4, 4, white);

    for (const Variant& var : variants) {
        reset_state();
        uint64_t t0 = plat::now_us();
        GLuint fbtex = 0, rb = 0, fbo = 0;
        glGenTextures(1, &fbtex);
        glBindTexture(GL_TEXTURE_2D, fbtex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 480, 272, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glGenRenderbuffers(1, &rb);
        glBindRenderbuffer(GL_RENDERBUFFER, rb);
        glRenderbufferStorage(GL_RENDERBUFFER, var.ds_format, 480, 272);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbtex, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, var.ds_attach, GL_RENDERBUFFER, rb);
        GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        double create_ms = (double)(plat::now_us() - t0) / 1000.0;
        if (st != GL_FRAMEBUFFER_COMPLETE) {
            fail("T7", var.name, "framebuffer incomplete: status 0x%04X (glError 0x%04X)", (unsigned)st, (unsigned)glGetError());
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            continue;
        }
        // ---- draw into the FBO: clear, near red quad, far green quad (depth test), a blue quad in the corner
        glViewport(0, 0, 480, 272);
        glBindTexture(GL_TEXTURE_2D, tex_white);
        clear_all(0.1f, 0.2f, 0.3f, 1.0f);
        glEnable(GL_DEPTH_TEST);
        draw_quad(-0.6f, -0.6f, 0.2f, 0.6f, -0.5f, RGBA(255, 0, 0));
        draw_quad(-0.2f, -0.6f, 0.6f, 0.6f, 0.5f, RGBA(0, 255, 0));
        glDisable(GL_DEPTH_TEST);
        draw_quad(0.7f, 0.7f, 0.95f, 0.95f, 0.0f, RGBA(0, 0, 255));
        // FBO coordinates: 480x272 window
        auto fp = [&](float nx, float ny) { return read_px(px_x(nx, 480), px_y(ny, 272)); };
        Px f_red = fp(-0.4f, 0.0f), f_ovl = fp(0.0f, 0.0f), f_green = fp(0.4f, 0.0f), f_blue = fp(0.82f, 0.82f), f_bg = fp(-0.9f, -0.9f);
        bool fbo_ok = near_px(f_red, 255, 0, 0) && near_px(f_ovl, 255, 0, 0) && near_px(f_green, 0, 255, 0) && near_px(f_blue, 0, 0, 255) && near_px(f_bg, 26, 51, 77, 4);
        check(fbo_ok, "T7", var.name, "480x272 RGBA8 texture + %s: complete in %.2f ms; readback from the FBO: red %s, overlap %s (red = depth ok), green %s, blue %s, background %s (~(26,51,77))",
              var.ds_format == GL_DEPTH_COMPONENT16 ? "DEPTH_COMPONENT16" : "DEPTH24_STENCIL8", create_ms, fmt_px(f_red, b1, sizeof b1), fmt_px(f_ovl, b2, sizeof b2), fmt_px(f_green, b3, sizeof b3), fmt_px(f_blue, b4, sizeof b4), fmt_px(f_bg, b1, sizeof b1));

        // ---- back to the screen: sample the FBO texture into the 960x544 default framebuffer
        uint64_t s0 = plat::now_us();
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, W, H);
        clear_all(0, 0, 0, 1);
        glBindTexture(GL_TEXTURE_2D, fbtex);
        draw_quad(-1, -1, 1, 1, 0.0f, RGBA(255, 255, 255));
        double switch_ms = (double)(plat::now_us() - s0) / 1000.0;
        Px s_red = read_px(px_x(-0.4f), px_y(0.0f)), s_green = read_px(px_x(0.4f), px_y(0.0f)), s_blue = read_px(px_x(0.82f), px_y(0.82f)), s_bg = read_px(px_x(-0.9f), px_y(-0.9f));
        bool dir_ok = near_px(s_red, 255, 0, 0, 10) && near_px(s_green, 0, 255, 0, 10) && near_px(s_blue, 0, 0, 255, 10) && near_px(s_bg, 26, 51, 77, 8);
        Px v_blue = read_px(px_x(0.82f), px_y(-0.82f));   // would hold the blue square if the texture arrived V-flipped
        bool vflip = near_px(v_blue, 0, 0, 255, 10) && !near_px(s_blue, 0, 0, 255, 10);
        if (dir_ok) {
            pass("T7", "fbo-sample", "FBO texture sampled 2x into the default framebuffer, orientation as in GL (not flipped): red %s green %s blue %s bg %s; switching back + draw took %.2f ms CPU",
                 fmt_px(s_red, b1, sizeof b1), fmt_px(s_green, b2, sizeof b2), fmt_px(s_blue, b3, sizeof b3), fmt_px(s_bg, b4, sizeof b4), switch_ms);
        } else if (vflip) {
            limit("T7", "fbo-sample", "FBO texture arrives V-FLIPPED when sampled (blue square found at the bottom right): vitaGL normally keeps FBO textures in GL orientation (only HAVE_UNFLIPPED_FBOS=1 leaves them flipped), so this library was built with that flag or the revision differs; flip V in the sampling shader or rebuild without it. red %s green %s",
                  fmt_px(s_red, b1, sizeof b1), fmt_px(s_green, b2, sizeof b2));
        } else {
            fail("T7", "fbo-sample", "FBO texture sampled into the default framebuffer shows unexpected colours: red %s green %s blue %s bg %s", fmt_px(s_red, b1, sizeof b1), fmt_px(s_green, b2, sizeof b2), fmt_px(s_blue, b3, sizeof b3), fmt_px(s_bg, b4, sizeof b4));
        }
        plat::gl_swap();
        poll_abort("T7");

        // ---- mid-frame FBO round trip keeps the default framebuffer's depth? (informational, decides STORE_DEPTH_STENCIL)
        {
            reset_state();
            glBindTexture(GL_TEXTURE_2D, tex_white);
            clear_all(0, 0, 0, 1);
            glEnable(GL_DEPTH_TEST);
            draw_quad(-0.5f, -0.5f, 0.5f, 0.5f, -0.5f, RGBA(255, 0, 0));
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glViewport(0, 0, 480, 272);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            draw_quad(-0.5f, -0.5f, 0.5f, 0.5f, 0.0f, RGBA(255, 255, 255));
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, W, H);
            glEnable(GL_DEPTH_TEST);
            draw_quad(-0.25f, -0.25f, 0.75f, 0.75f, 0.5f, RGBA(0, 255, 0));
            Px ov = read_px(px_x(0.0f), px_y(0.0f));
            if (near_px(ov, 255, 0, 0)) pass("T7", "depth-across-fbo-switch", "default-framebuffer depth survived an FBO round trip (overlap %s red)", fmt_px(ov, b1, sizeof b1));
            else limit("T7", "depth-across-fbo-switch", "default-framebuffer depth was LOST across an FBO round trip (overlap %s); build vitaGL with STORE_DEPTH_STENCIL=1 (libvitaGL_store.a) if the port needs it", fmt_px(ov, b1, sizeof b1));
            plat::gl_swap();
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
        glDeleteRenderbuffers(1, &rb);
        glDeleteTextures(1, &fbtex);
        glGetError();
    }

    // ---- limits: how many FBOs / what sizes
    {
        GLuint fb[16], tx[16];
        int made = 0;
        glGenTextures(16, tx);
        glGenFramebuffers(16, fb);
        for (int i = 0; i < 16; i++) {
            glBindTexture(GL_TEXTURE_2D, tx[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 480, 272, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glBindFramebuffer(GL_FRAMEBUFFER, fb[i]);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tx[i], 0);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) made++;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(16, fb);
        glDeleteTextures(16, tx);
        glGetError();
        info("T7", "fbo-count", "%d of 16 colour-only 480x272 FBOs complete at the same time", made);
    }
    {
        info("T7", "pools", "vitaGL pools free: ram %u KiB, vram %u KiB, phycont %u KiB", (unsigned)(vglMemFree(VGL_MEM_RAM) / 1024), (unsigned)(vglMemFree(VGL_MEM_VRAM) / 1024), (unsigned)(vglMemFree(VGL_MEM_PHYCONT) / 1024));
    }
    glDeleteTextures(1, &tex_white);
    gl_errors("T7", "all T7 checks");
}

// ================================================================================================================ T8
namespace {

struct T8Stats {
    double submit_avg = 0, submit_p95 = 0, submit_max = 0, upload_avg = 0, swap_avg = 0, swap_max = 0, wall_avg = 0, wall_p95 = 0;
    bool ok = false;
};

double percentile(double* v, int n, double p) {
    for (int i = 1; i < n; i++) { double x = v[i]; int j = i - 1; while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; j--; } v[j + 1] = x; }
    int idx = (int)(p * (n - 1) + 0.5);
    return v[idx < 0 ? 0 : (idx >= n ? n - 1 : idx)];
}

enum class Src { Client, VboFirst, VboRepoint, VboStream, VboFirstBlendChurn };

struct T8Ctx {
    int frames = 60, warmup = 5, draws = 700, verts = 28, period = 50;
    Vtx* data = nullptr;          // draws * verts vertices, built once
    GLuint vbo[4] = {0, 0, 0, 0};
    GLuint stream_vbo = 0;
    GLuint texs[16];
    GLuint fbo = 0, fbtex = 0, rb = 0;
    Prog progs[2];
    size_t vbytes = 0;
};

void t8_build_data(T8Ctx& c) {
    c.vbytes = (size_t)c.draws * c.verts * sizeof(Vtx);
    c.data = (Vtx*)memalign(64, c.vbytes);
    const int cols = 35;
    int rows = (c.draws + cols - 1) / cols;
    for (int s = 0; s < c.draws; s++) {
        float cx = -1.0f + 2.0f * (float)(s % cols) / (float)cols;
        float cy = -1.0f + 2.0f * (float)(s / cols) / (float)rows;
        float cw = 2.0f / cols * 0.95f, ch = 2.0f / rows * 0.7f;
        float z = 0.9f - 0.0005f * (float)(s % 1000);
        Vtx* v = c.data + (size_t)s * c.verts;
        for (int i = 0; i < c.verts; i++) {
            int seg = i / 2;
            float fx = (float)seg / (float)(c.verts / 2 - 1);
            v[i].x = cx + fx * cw;
            v[i].y = cy + (float)(i & 1) * ch;
            v[i].z = z;
            v[i].u = fx;
            v[i].v = (float)(i & 1);
            v[i].r = (uint8_t)(80 + (s * 7) % 170);
            v[i].g = (uint8_t)(80 + (s * 13) % 170);
            v[i].b = (uint8_t)(80 + (s * 29) % 170);
            v[i].a = (uint8_t)(150 + (s % 100));
        }
    }
}

bool t8_setup(T8Ctx& c, bool need_fbo) {
    char err[512];
    if (!build_prog(c.progs[0], kVS, kFS_tex, 777, err, sizeof(err))) {
        if (!plat::gl_shader_compiler_available()) limit("T8", "shader", "libshacccg.suprx is missing (Vita3K has no shader compiler either): no GLSL can be built, T8 skipped. %.120s", err);
        else fail("T8", "shader", "program A: %.200s", err);
        return false;
    }
    if (!build_prog(c.progs[1], kVS, kFS_tex2, 778, err, sizeof(err))) {
        if (!plat::gl_shader_compiler_available()) limit("T8", "shader", "libshacccg.suprx is missing: program B not built, T8 skipped. %.120s", err);
        else fail("T8", "shader", "program B: %.200s", err);
        return false;
    }
    for (int i = 0; i < 16; i++) {
        uint8_t px[64 * 64 * 4];
        for (int k = 0; k < 64 * 64; k++) {
            px[k * 4 + 0] = (uint8_t)(60 + i * 12);
            px[k * 4 + 1] = (uint8_t)(255 - i * 10 + ((k >> 3) & 15));
            px[k * 4 + 2] = (uint8_t)(((k & 63) * 4 + i * 16) & 255);
            px[k * 4 + 3] = 255;
        }
        c.texs[i] = make_tex_rgba(64, 64, px, GL_LINEAR);
    }
    glGenBuffers(4, c.vbo);
    for (int i = 0; i < 4; i++) {
        glBindBuffer(GL_ARRAY_BUFFER, c.vbo[i]);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)c.vbytes, c.data, GL_DYNAMIC_DRAW);   // allocate once; refilled with glBufferSubData
    }
    glGenBuffers(1, &c.stream_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    if (need_fbo) {
        glGenTextures(1, &c.fbtex);
        glBindTexture(GL_TEXTURE_2D, c.fbtex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 480, 272, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glGenRenderbuffers(1, &c.rb);
        glBindRenderbuffer(GL_RENDERBUFFER, c.rb);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, 480, 272);
        glGenFramebuffers(1, &c.fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, c.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c.fbtex, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, c.rb);
        bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (!ok) { fail("T8", "fbo", "FBO incomplete"); return false; }
    }
    return true;
}

T8Stats t8_run(T8Ctx& c, Src src, bool uniforms, bool fbo_switch, const char* label) {
    T8Stats st;
    static double submit[512], swap[512], wall[512], upload[512];
    if (c.frames > 512) c.frames = 512;
    int total = c.frames + c.warmup;
    const float tints[16][4] = {{1, 1, 1, 1}, {0.9f, 1, 1, 1}, {1, 0.9f, 1, 1}, {1, 1, 0.9f, 1}, {0.8f, 0.8f, 1, 1}, {1, 0.8f, 0.8f, 1}, {0.8f, 1, 0.8f, 1}, {1, 1, 1, 0.9f},
                                {0.7f, 1, 1, 1}, {1, 0.7f, 1, 1}, {1, 1, 0.7f, 1}, {0.6f, 0.6f, 1, 1}, {1, 0.6f, 0.6f, 1}, {0.6f, 1, 0.6f, 1}, {1, 1, 1, 0.8f}, {0.95f, 0.95f, 0.95f, 1}};
    uint64_t prev_end = plat::now_us();
    int meas = 0;
    for (int f = 0; f < total; f++) {
        uint64_t t_start = plat::now_us();
        uint64_t upload_us = 0;
        reset_state();
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_DEPTH_TEST);
        glActiveTexture(GL_TEXTURE0);

        GLuint ring = c.vbo[f & 3];
        if (src == Src::VboFirst || src == Src::VboRepoint || src == Src::VboFirstBlendChurn) {
            uint64_t u0 = plat::now_us();
            glBindBuffer(GL_ARRAY_BUFFER, ring);
            glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)c.vbytes, c.data);
            upload_us = plat::now_us() - u0;
        } else if (src == Src::VboStream) {
            uint64_t u0 = plat::now_us();
            glBindBuffer(GL_ARRAY_BUFFER, c.stream_vbo);
            glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)c.vbytes, c.data, GL_STREAM_DRAW);   // orphan + refill every frame
            upload_us = plat::now_us() - u0;
        }
        int cur_prog = -1;
        auto select = [&](int which) {
            if (which == cur_prog) return;
            cur_prog = which;
            use_prog(c.progs[which]);
        };
        if (src == Src::VboFirst || src == Src::VboFirstBlendChurn || src == Src::VboStream) {
            GLuint b = src == Src::VboStream ? c.stream_vbo : ring;
            set_attribs_vbo(b, 0);
        }

        int fbo_draws = fbo_switch ? c.draws / 7 : 0;
        if (fbo_switch) {
            glBindFramebuffer(GL_FRAMEBUFFER, c.fbo);
            glViewport(0, 0, 480, 272);
            glClearColor(0.1f, 0.1f, 0.2f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        } else {
            glClearColor(0.1f, 0.1f, 0.2f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        }
        bool in_fbo = fbo_switch;
        bool blend_mode = false;
        for (int s = 0; s < c.draws; s++) {
            if (in_fbo && s == fbo_draws) {
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glViewport(0, 0, W, H);
                glClearColor(0.1f, 0.1f, 0.2f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                in_fbo = false;
            }
            select((s / c.period) & 1);
            glBindTexture(GL_TEXTURE_2D, c.texs[(s * 7) & 15]);
            if (uniforms) glUniform4fv(c.progs[cur_prog].u_tint, 1, tints[s & 15]);
            if (src == Src::VboFirstBlendChurn && (s & 7) == 0) {
                blend_mode = !blend_mode;
                if (blend_mode) glBlendFunc(GL_ONE, GL_ONE); else glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            }
            switch (src) {
            case Src::Client:
                set_attribs_client(c.data + (size_t)s * c.verts);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, c.verts);
                break;
            case Src::VboFirst:
            case Src::VboFirstBlendChurn:
            case Src::VboStream:
                glDrawArrays(GL_TRIANGLE_STRIP, s * c.verts, c.verts);
                break;
            case Src::VboRepoint:
                set_attribs_vbo(ring, (size_t)s * c.verts * sizeof(Vtx));
                glDrawArrays(GL_TRIANGLE_STRIP, 0, c.verts);
                break;
            }
        }
        if (fbo_switch) {   // consume the FBO result so the dependency is real
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            select(0);
            glDisable(GL_DEPTH_TEST);
            glBindTexture(GL_TEXTURE_2D, c.fbtex);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            const float tint[4] = {1, 1, 1, 0.25f};
            glUniform4fv(c.progs[0].u_tint, 1, tint);
            draw_quad(-1, -1, 1, 1, 0.0f, RGBA(255, 255, 255, 255));
        }
        uint64_t t_sub_end = plat::now_us();
        plat::gl_swap();
        uint64_t t_swap_end = plat::now_us();
        if (f >= c.warmup && meas < 512) {
            submit[meas] = (double)(t_sub_end - t_start) / 1000.0;
            upload[meas] = (double)upload_us / 1000.0;
            swap[meas] = (double)(t_swap_end - t_sub_end) / 1000.0;
            wall[meas] = (double)(t_swap_end - prev_end) / 1000.0;
            meas++;
        }
        prev_end = t_swap_end;
        if ((f & 15) == 15) poll_abort("T8");
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    if (meas == 0) return st;
    double ss = 0, us = 0, ws = 0, wl = 0, smax = 0, wmax = 0;
    for (int i = 0; i < meas; i++) {
        ss += submit[i]; us += upload[i]; ws += swap[i]; wl += wall[i];
        if (submit[i] > smax) smax = submit[i];
        if (swap[i] > wmax) wmax = swap[i];
    }
    st.submit_avg = ss / meas; st.upload_avg = us / meas; st.swap_avg = ws / meas; st.wall_avg = wl / meas; st.submit_max = smax; st.swap_max = wmax;
    st.submit_p95 = percentile(submit, meas, 0.95);
    st.wall_p95 = percentile(wall, meas, 0.95);
    st.ok = true;
    GLenum e = glGetError();
    check(e == GL_NO_ERROR, "T8", label,
          "%d frames x %d draws x %d verts: CPU submit %.2f ms/frame (p95 %.2f, max %.2f) = %.2f us/draw [of which buffer upload %.2f ms]; swap %.2f ms (max %.2f); wall %.2f ms/frame (p95 %.2f) = %.1f fps; glError 0x%04X",
          meas, c.draws, c.verts, st.submit_avg, st.submit_p95, st.submit_max, st.submit_avg * 1000.0 / c.draws, st.upload_avg, st.swap_avg, st.swap_max, st.wall_avg, st.wall_p95, 1000.0 / st.wall_avg, (unsigned)e);
    return st;
}

}  // namespace

void t8_draw_cost() {
    if (!ensure_gl("T8")) return;
    status("T8 draw-call cost");
    glGetError();
    T8Ctx c;
    c.frames = cfg_int("t8.frames", 60);
    c.warmup = cfg_int("t8.warmup", 5);
    c.draws = cfg_int("t8.draws", 700);
    c.verts = cfg_int("t8.verts", 28);
    c.period = cfg_int("t8.period", 50);
    if (c.verts < 4 || (c.verts & 1)) c.verts = 28;
    if (c.draws < 1) c.draws = 1;
    const bool fbo_switch = cfg_int("t8.fbo", 1) != 0;
    plat::gl_set_overlay(nullptr);              // keep the CPU overlay out of the measurement
    bool vsync_was = cfg_int("gl.vsync", 1) != 0;
    plat::gl_set_vsync(cfg_int("t8.vsync", 0) != 0);   // swap time then shows queueing/GPU back-pressure, not the vblank wait
    t8_build_data(c);
    if (!t8_setup(c, fbo_switch)) { plat::gl_set_overlay(overlay_cb); plat::gl_set_vsync(vsync_was); return; }
    info("T8", "config", "frames %d (+%d warm-up), %d draws/frame, %d vertices per strip (%zu bytes/strip, %zu KiB vertex data/frame), program switch every %d draws, texture bind + %s per draw, FBO switch %s, depth test + alpha blend on, vsync %s",
         c.frames, c.warmup, c.draws, c.verts, c.verts * sizeof(Vtx), c.vbytes / 1024, c.period, "one vec4 uniform update (when enabled)", fbo_switch ? "once per frame (480x272 + depth, consumed by a final fullscreen quad)" : "off",
         cfg_int("t8.vsync", 0) ? "on" : "off");

#ifdef ST_VGL_FAST
    limit("T8", "client-arrays", "skipped: this is the DRAW_SPEEDHACK library (libvitaGL_fast.a); client arrays must point to GPU-mapped memory there, a plain heap pointer would fault the GPU");
    limit("T8", "client-arrays-nouniform", "skipped (DRAW_SPEEDHACK)");
#else
    T8Stats a = t8_run(c, Src::Client, true, fbo_switch, "client-arrays+uniforms");
    T8Stats b = t8_run(c, Src::Client, false, fbo_switch, "client-arrays");
#endif
    T8Stats d = t8_run(c, Src::VboFirst, true, fbo_switch, "vbo-ring-first+uniforms");
    T8Stats e = t8_run(c, Src::VboFirst, false, fbo_switch, "vbo-ring-first");
    T8Stats f = t8_run(c, Src::VboRepoint, true, fbo_switch, "vbo-ring-repoint+uniforms");
    T8Stats g = t8_run(c, Src::VboStream, true, fbo_switch, "vbo-stream-orphan+uniforms");
    T8Stats h = t8_run(c, Src::VboFirstBlendChurn, true, fbo_switch, "vbo-ring-first+uniforms+blend-churn");
    if (cfg_int("t8.nofbo_compare", 1) && fbo_switch) {
        T8Stats n = t8_run(c, Src::VboFirst, true, false, "vbo-ring-first+uniforms (no FBO switch)");
        if (n.ok && d.ok) info("T8", "fbo-switch-cost", "one FBO round trip per frame costs about %.2f ms of CPU submit and %.2f ms of wall time per frame", d.submit_avg - n.submit_avg, d.wall_avg - n.wall_avg);
    }
#ifndef ST_VGL_FAST
    if (a.ok && b.ok) info("T8", "uniform-cost", "per-draw vec4 uniform update (client arrays): +%.2f ms/frame = %.2f us/draw", a.submit_avg - b.submit_avg, (a.submit_avg - b.submit_avg) * 1000.0 / c.draws);
    if (a.ok && d.ok) info("T8", "client-vs-vbo", "client arrays %.2f ms vs VBO(first) %.2f ms CPU submit per frame -> VBO is %.1fx cheaper to submit", a.submit_avg, d.submit_avg, d.submit_avg > 0 ? a.submit_avg / d.submit_avg : 0.0);
#endif
    if (d.ok && e.ok) info("T8", "uniform-cost-vbo", "per-draw vec4 uniform update (VBO): +%.2f ms/frame = %.2f us/draw", d.submit_avg - e.submit_avg, (d.submit_avg - e.submit_avg) * 1000.0 / c.draws);
    if (d.ok && f.ok) info("T8", "repoint-cost", "re-pointing the 3 attributes per draw (offsets) instead of varying `first`: +%.2f ms/frame", f.submit_avg - d.submit_avg);
    if (d.ok && g.ok) info("T8", "stream-vs-ring", "glBufferData(GL_STREAM_DRAW) orphaning vs ring + glBufferSubData: %.2f vs %.2f ms upload+submit per frame", g.submit_avg, d.submit_avg);
    if (d.ok && h.ok) info("T8", "blend-churn-cost", "changing glBlendFunc every 8 draws: +%.2f ms/frame", h.submit_avg - d.submit_avg);
    if (d.ok) info("T8", "budget", "best path (VBO ring, first=, uniforms): %.2f ms CPU submit + %.2f ms swap per frame; a 30 fps frame budget is 33.3 ms, 60 fps 16.7 ms. The same %d draws/frame use %.0f%% of a 33 ms frame on the GL core.",
                   d.submit_avg, d.swap_avg, c.draws, (d.submit_avg + d.swap_avg) / 33.3 * 100.0);
    info("T8", "pools", "vitaGL pools free after T8: ram %u KiB, vram %u KiB", (unsigned)(vglMemFree(VGL_MEM_RAM) / 1024), (unsigned)(vglMemFree(VGL_MEM_VRAM) / 1024));
    plat::gl_set_vsync(vsync_was);
    plat::gl_set_overlay(overlay_cb);
    free(c.data);
}

}  // namespace st
