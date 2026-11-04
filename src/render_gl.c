/* psprecomp -- the OpenGL 3.3 render backend. See docs/RENDERER.md.
 *
 * Runs entirely on the GE's thread, which owns the context, and presents straight
 * to the host window. Every framebuffer the game draws to becomes a texture with a
 * depth buffer, at a chosen multiple of the PSP's resolution. That is what lets 3D
 * come out sharper than the console could draw it, and a game drawing into a buffer
 * and then using it as a texture just works.
 *
 * The cost of that: pixels the GPU draws are not in guest VRAM, so a game reading
 * its own framebuffer back with the CPU sees stale data. Nothing has needed it yet.
 *
 * Positions arrive in homogeneous screen space, so the GPU clips what crosses the
 * camera and interpolates textures with perspective, which the software path does
 * not do. Everything else is meant to match it, and that path remains the reference.
 */

#include "psprecomp/render.h"
#include "psprecomp/hle.h"
#include "psprecomp/mem_fast.h"
#include "psprecomp/window.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#include <GL/glext.h>

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Targets are all this size in guest pixels, whatever the game's stride. */
#define LOGICAL 512
#define PSP_W   480
#define PSP_H   272

#define MAX_TARGETS   16
#define TEX_SLOTS     2048
#define TEX_MAX_LIVE  1024
#define TEX_MAX_SIDE  1024
#define HASH_SAMPLES  4096       /* words of a texture checked for changes per frame */
#define SNAPSHOT_EVERY 15        /* presents between copies kept for capture */

#define GL_FUNCS(X) \
    X(PFNGLCREATESHADERPROC, glCreateShader) \
    X(PFNGLSHADERSOURCEPROC, glShaderSource) \
    X(PFNGLCOMPILESHADERPROC, glCompileShader) \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv) \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram) \
    X(PFNGLATTACHSHADERPROC, glAttachShader) \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram) \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv) \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog) \
    X(PFNGLUSEPROGRAMPROC, glUseProgram) \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) \
    X(PFNGLUNIFORM1IPROC, glUniform1i) \
    X(PFNGLUNIFORM2FPROC, glUniform2f) \
    X(PFNGLUNIFORM3FPROC, glUniform3f) \
    X(PFNGLGENBUFFERSPROC, glGenBuffers) \
    X(PFNGLBINDBUFFERPROC, glBindBuffer) \
    X(PFNGLBUFFERDATAPROC, glBufferData) \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray) \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) \
    X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer) \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers) \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer) \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D) \
    X(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer) \
    X(PFNGLGENRENDERBUFFERSPROC, glGenRenderbuffers) \
    X(PFNGLBINDRENDERBUFFERPROC, glBindRenderbuffer) \
    X(PFNGLRENDERBUFFERSTORAGEPROC, glRenderbufferStorage) \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus) \
    X(PFNGLBLITFRAMEBUFFERPROC, glBlitFramebuffer) \
    X(PFNGLBLENDEQUATIONSEPARATEPROC, glBlendEquationSeparate) \
    X(PFNGLBLENDFUNCSEPARATEPROC, glBlendFuncSeparate) \
    X(PFNGLBLENDCOLORPROC, glBlendColor)

#define X(type, name) static type p_##name;
GL_FUNCS(X)
#undef X

static const char VERTEX_SHADER[] =
    "#version 330 core\n"
    "layout(location = 0) in vec4 a_pos;\n"
    "layout(location = 1) in vec4 a_col;\n"
    "layout(location = 2) in vec2 a_uv;\n"
    "uniform vec2 u_uvscale;\n"
    "out vec4 v_col;\n"
    "flat out vec4 v_flat;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    /* Pixels (times w) to clip space. Depth 0..65535 becomes the window's 0..1. */
    "    float w = a_pos.w;\n"
    "    gl_Position = vec4(a_pos.x / 256.0 - w, a_pos.y / 256.0 - w, a_pos.z / 32767.5 - w, w);\n"
    "    v_col = a_col;\n"
    "    v_flat = a_col;\n"
    "    v_uv = a_uv * u_uvscale;\n"
    "}\n";

/* The texture functions and alpha test are those of tex_combine and alpha_pass in
 * render.c. */
static const char FRAGMENT_SHADER[] =
    "#version 330 core\n"
    "in vec4 v_col;\n"
    "flat in vec4 v_flat;\n"
    "in vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "uniform int u_tex_on, u_func, u_talpha, u_double, u_gouraud;\n"
    "uniform int u_afunc, u_aref, u_amask;\n"
    "uniform vec3 u_env, u_sfix;\n"
    "out vec4 o_col;\n"
    "void main() {\n"
    "    vec4 c = u_gouraud != 0 ? v_col : v_flat;\n"
    "    if (u_tex_on != 0) {\n"
    "        vec4 t = texture(u_tex, v_uv);\n"
    "        float a = u_talpha != 0 ? t.a * c.a : c.a;\n"
    "        vec3 rgb;\n"
    "        if (u_func == 0) rgb = t.rgb * c.rgb;\n"
    "        else if (u_func == 1) rgb = u_talpha != 0 ? mix(c.rgb, t.rgb, t.a) : t.rgb;\n"
    "        else if (u_func == 2) rgb = mix(c.rgb, u_env, t.rgb);\n"
    "        else if (u_func == 3) rgb = t.rgb;\n"
    "        else rgb = t.rgb + c.rgb;\n"
    "        if (u_func == 1) a = c.a;\n"
    "        else if (u_func == 3) a = u_talpha != 0 ? t.a : c.a;\n"
    "        if (u_double != 0) rgb *= 2.0;\n"
    "        c = vec4(clamp(rgb, 0.0, 1.0), a);\n"
    "    }\n"
    "    if (u_afunc >= 0) {\n"
    "        int a = int(c.a * 255.0 + 0.5) & u_amask;\n"
    "        int r = u_aref & u_amask;\n"
    "        bool ok = u_afunc == 1;\n"
    "        if (u_afunc == 2) ok = a == r;\n"
    "        else if (u_afunc == 3) ok = a != r;\n"
    "        else if (u_afunc == 4) ok = a < r;\n"
    "        else if (u_afunc == 5) ok = a <= r;\n"
    "        else if (u_afunc == 6) ok = a > r;\n"
    "        else if (u_afunc == 7) ok = a >= r;\n"
    "        if (!ok) discard;\n"
    "    }\n"
    "    o_col = vec4(c.rgb * u_sfix, c.a);\n"
    "}\n";

typedef struct { float x, y, z, w; uint32_t rgba; float u, v; } gl_vertex;

typedef struct {
    int used;
    uint32_t addr;
    GLuint fbo, color, depth;
    uint64_t last_used;
} gl_target;

typedef struct {
    int used;
    uint32_t addr;
    int w, h, fmt, bw, swizzle;
    uint32_t clut_hash, data_hash;
    uint64_t checked_frame;
    GLuint tex;
    int min_linear, mag_linear, clamp_u, clamp_v;
} gl_texture;

static struct {
    GLint tex_on, func, talpha, dbl, gouraud, afunc, aref, amask, env, sfix, uvscale;
} U;

static HDC    g_dc;
static int    g_scale = 2;
static GLuint g_program, g_vao, g_vbo;
static GLuint g_vram_fbo, g_vram_tex, g_small_fbo, g_small_tex;

static gl_target  g_targets[MAX_TARGETS];
static gl_target *g_bound;
static uint32_t   g_target_addr;
static gl_texture g_textures[TEX_SLOTS];
static int        g_live_textures;
static uint64_t   g_frame = 1, g_tick;

static gl_vertex  g_vb[65536 * 3];
static uint32_t   g_decode[TEX_MAX_SIDE * TEX_MAX_SIDE];
static uint32_t   g_upload[PSP_W * PSP_H];
static uint32_t   g_snapshot[PSP_W * PSP_H];
static volatile int g_have_snapshot;

static int g_overlay;

void psp_render_gl_set_overlay(int on) { g_overlay = on; }

void psp_render_gl_set_scale(int scale) {
    if (scale >= 1 && scale <= 8) g_scale = scale;
}

/* Addresses compare within VRAM, whichever mirror a game names them through. */
static uint32_t vram_offset(uint32_t addr) { return addr & (PSP_VRAM_SIZE - 1); }
static int in_vram(uint32_t addr) {
    return ((addr & PSP_ADDR_MASK) - PSP_VRAM_BASE) < PSP_VRAM_SIZE;
}

/* ---- setup --------------------------------------------------------------- */

static GLuint compile(GLenum kind, const char *src) {
    const GLuint s = p_glCreateShader(kind);
    p_glShaderSource(s, 1, &src, NULL);
    p_glCompileShader(s);
    GLint ok = 0;
    p_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = { 0 };
        p_glGetShaderInfoLog(s, sizeof log - 1, NULL, log);
        fprintf(stderr, "gl: shader failed:\n%s\n", log);
        return 0;
    }
    return s;
}

static int build_program(void) {
    const GLuint vs = compile(GL_VERTEX_SHADER, VERTEX_SHADER);
    const GLuint fs = compile(GL_FRAGMENT_SHADER, FRAGMENT_SHADER);
    if (!vs || !fs) return -1;
    g_program = p_glCreateProgram();
    p_glAttachShader(g_program, vs);
    p_glAttachShader(g_program, fs);
    p_glLinkProgram(g_program);
    GLint ok = 0;
    p_glGetProgramiv(g_program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = { 0 };
        p_glGetProgramInfoLog(g_program, sizeof log - 1, NULL, log);
        fprintf(stderr, "gl: link failed:\n%s\n", log);
        return -1;
    }
    p_glUseProgram(g_program);
    U.tex_on  = p_glGetUniformLocation(g_program, "u_tex_on");
    U.func    = p_glGetUniformLocation(g_program, "u_func");
    U.talpha  = p_glGetUniformLocation(g_program, "u_talpha");
    U.dbl     = p_glGetUniformLocation(g_program, "u_double");
    U.gouraud = p_glGetUniformLocation(g_program, "u_gouraud");
    U.afunc   = p_glGetUniformLocation(g_program, "u_afunc");
    U.aref    = p_glGetUniformLocation(g_program, "u_aref");
    U.amask   = p_glGetUniformLocation(g_program, "u_amask");
    U.env     = p_glGetUniformLocation(g_program, "u_env");
    U.sfix    = p_glGetUniformLocation(g_program, "u_sfix");
    U.uvscale = p_glGetUniformLocation(g_program, "u_uvscale");
    return 0;
}

static GLuint make_color_target(GLuint *tex, int w, int h, GLenum filter) {
    GLuint fbo;
    glGenTextures(1, tex);
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    p_glGenFramebuffers(1, &fbo);
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
    return fbo;
}

/* The context has to be created and used on one thread, and that is the GE's. */
static int gl_thread_init(void) {
    HWND window = (HWND)psp_window_handle();
    if (!window) { fprintf(stderr, "gl: no window\n"); return -1; }
    g_dc = GetDC(window);

    PIXELFORMATDESCRIPTOR pfd;
    memset(&pfd, 0, sizeof pfd);
    pfd.nSize = sizeof pfd;
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    const int format = ChoosePixelFormat(g_dc, &pfd);
    if (!format || !SetPixelFormat(g_dc, format, &pfd)) {
        fprintf(stderr, "gl: no pixel format\n");
        return -1;
    }
    HGLRC context = wglCreateContext(g_dc);
    if (!context || !wglMakeCurrent(g_dc, context)) {
        fprintf(stderr, "gl: no context\n");
        return -1;
    }

    const char *version = (const char *)glGetString(GL_VERSION);
    int major = 0, minor = 0;
    if (version) sscanf(version, "%d.%d", &major, &minor);
    fprintf(stderr, "gl: %s, %s, render scale %d\n", (const char *)glGetString(GL_RENDERER),
            version ? version : "(unknown)", g_scale);
    if (major < 3 || (major == 3 && minor < 3)) {
        fprintf(stderr, "gl: OpenGL 3.3 is needed\n");
        return -1;
    }

#define X(type, name) \
    p_##name = (type)(void *)wglGetProcAddress(#name); \
    if (!p_##name) { fprintf(stderr, "gl: missing " #name "\n"); return -1; }
    GL_FUNCS(X)
#undef X

    /* Pacing is the game's. Waiting for the monitor here would stall the GE. */
    typedef BOOL (WINAPI *swap_interval_fn)(int);
    swap_interval_fn swap_interval = (swap_interval_fn)(void *)wglGetProcAddress("wglSwapIntervalEXT");
    if (swap_interval) swap_interval(0);

    if (build_program() != 0) return -1;

    p_glGenVertexArrays(1, &g_vao);
    p_glBindVertexArray(g_vao);
    p_glGenBuffers(1, &g_vbo);
    p_glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    p_glEnableVertexAttribArray(0);
    p_glEnableVertexAttribArray(1);
    p_glEnableVertexAttribArray(2);
    p_glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(gl_vertex), (void *)offsetof(gl_vertex, x));
    p_glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(gl_vertex), (void *)offsetof(gl_vertex, rgba));
    p_glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(gl_vertex), (void *)offsetof(gl_vertex, u));

    g_vram_fbo = make_color_target(&g_vram_tex, PSP_W, PSP_H, GL_NEAREST);
    g_small_fbo = make_color_target(&g_small_tex, PSP_W, PSP_H, GL_NEAREST);

    /* Depth outside its range is clamped, not clipped: the GE keeps such pixels. */
    glEnable(0x864F /* GL_DEPTH_CLAMP */);
    glDisable(GL_DITHER);
    glCullFace(GL_BACK);
    return 0;
}

/* ---- targets ------------------------------------------------------------- */

static gl_target *find_target(uint32_t addr) {
    const uint32_t off = vram_offset(addr);
    for (int i = 0; i < MAX_TARGETS; i++)
        if (g_targets[i].used && g_targets[i].addr == off) return &g_targets[i];
    return NULL;
}

static gl_target *get_target(uint32_t addr) {
    gl_target *t = find_target(addr);
    if (t) return t;

    t = &g_targets[0];
    for (int i = 0; i < MAX_TARGETS; i++) {
        if (!g_targets[i].used) { t = &g_targets[i]; break; }
        if (g_targets[i].last_used < t->last_used) t = &g_targets[i];
    }
    if (!t->used) {
        const int side = LOGICAL * g_scale;
        t->fbo = make_color_target(&t->color, side, side, GL_LINEAR);
        p_glGenRenderbuffers(1, &t->depth);
        p_glBindRenderbuffer(GL_RENDERBUFFER, t->depth);
        p_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, side, side);
        p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, t->depth);
        if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            fprintf(stderr, "gl: incomplete framebuffer\n");
    }
    t->used = 1;
    t->addr = vram_offset(addr);

    p_glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glClearColor(0, 0, 0, 0);
    glClearDepth(0.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    g_bound = NULL;
    return t;
}

static void bind_target(void) {
    gl_target *t = get_target(g_target_addr);
    t->last_used = ++g_tick;
    if (g_bound == t) return;
    p_glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glViewport(0, 0, LOGICAL * g_scale, LOGICAL * g_scale);
    g_bound = t;
}

/* ---- textures ------------------------------------------------------------ */

static uint32_t hash_words(const uint8_t *p, size_t bytes, size_t max_words) {
    size_t words = bytes / 4;
    const size_t step = words > max_words ? words / max_words : 1;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < words; i += step) {
        uint32_t w;
        memcpy(&w, p + i * 4, 4);
        h = (h ^ w) * 16777619u;
    }
    return h;
}

static int is_indexed(int fmt) { return fmt >= PSP_TPSM_T4 && fmt <= PSP_TPSM_T32; }

static size_t texture_bytes(const psp_draw_state *st) {
    static const int BPP[11] = { 16, 16, 16, 32, 4, 8, 16, 32, 4, 8, 8 };
    const int fmt = st->tex_fmt >= 0 && st->tex_fmt <= 10 ? st->tex_fmt : 3;
    const int width = fmt >= PSP_TPSM_DXT1 ? st->tex_w : (st->tex_bw > st->tex_w ? st->tex_bw : st->tex_w);
    return (size_t)width * (size_t)st->tex_h * (size_t)BPP[fmt] / 8u;
}

static uint32_t texture_hash(const psp_draw_state *st) {
    size_t bytes = texture_bytes(st);
    const uint8_t *p = psp_fast_ptr(st->tex_addr, (uint32_t)bytes);
    return p ? hash_words(p, bytes, HASH_SAMPLES) : 0;
}

static uint64_t g_decoded, g_decode_us, g_decode_worst_us;

void psp_render_gl_texture_stats(uint64_t *count, uint64_t *total_us, uint64_t *worst_us) {
    *count = g_decoded;
    *total_us = g_decode_us;
    *worst_us = g_decode_worst_us;
}

static uint64_t gl_now_us(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (uint64_t)((double)now.QuadPart * 1e6 / (double)freq.QuadPart);
}

static void decode_texture(gl_texture *e, const psp_draw_state *st) {
    const int w = st->tex_w, h = st->tex_h;
    if (w > TEX_MAX_SIDE || h > TEX_MAX_SIDE) return;
    const uint64_t began = gl_now_us();
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            g_decode[y * w + x] = psp_tex_fetch(st, x, y);
    glBindTexture(GL_TEXTURE_2D, e->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, g_decode);
    const uint64_t took = gl_now_us() - began;
    g_decoded++;
    g_decode_us += took;
    if (took > g_decode_worst_us) g_decode_worst_us = took;
}

static void forget_textures(void) {
    for (int i = 0; i < TEX_SLOTS; i++) {
        if (g_textures[i].used) glDeleteTextures(1, &g_textures[i].tex);
        g_textures[i].used = 0;
    }
    g_live_textures = 0;
}

static gl_texture *get_texture(const psp_draw_state *st) {
    const uint32_t clut = is_indexed(st->tex_fmt) && st->clut ? hash_words(st->clut, 1024, 256) : 0;
    uint32_t slot = (st->tex_addr * 2654435761u) ^ clut ^ ((uint32_t)st->tex_w << 3) ^
                    ((uint32_t)st->tex_h << 13) ^ ((uint32_t)st->tex_fmt << 23);
    slot %= TEX_SLOTS;

    if (g_live_textures >= TEX_MAX_LIVE) forget_textures();
    gl_texture *e = NULL;
    for (int probe = 0; probe < TEX_SLOTS; probe++) {
        gl_texture *c = &g_textures[(slot + (uint32_t)probe) % TEX_SLOTS];
        if (!c->used) { e = c; break; }
        if (c->addr == st->tex_addr && c->w == st->tex_w && c->h == st->tex_h && c->fmt == st->tex_fmt &&
            c->bw == st->tex_bw && c->swizzle == st->tex_swizzle && c->clut_hash == clut) { e = c; break; }
    }
    if (!e) return NULL;

    if (!e->used) {
        memset(e, 0, sizeof *e);
        e->used = 1;
        e->addr = st->tex_addr;
        e->w = st->tex_w;
        e->h = st->tex_h;
        e->fmt = st->tex_fmt;
        e->bw = st->tex_bw;
        e->swizzle = st->tex_swizzle;
        e->clut_hash = clut;
        e->min_linear = e->mag_linear = e->clamp_u = e->clamp_v = -1;
        glGenTextures(1, &e->tex);
        g_live_textures++;
    }

    /* Worth looking at the bytes once a frame at most: a game changes a texture
     * between frames, and a big one would otherwise be read on every draw. */
    if (e->checked_frame != g_frame) {
        const uint32_t h = texture_hash(st);
        if (e->checked_frame == 0 || h != e->data_hash) {
            e->data_hash = h;
            decode_texture(e, st);
        }
        e->checked_frame = g_frame;
    }
    return e;
}

static void bind_sampler(GLuint tex, int *min_seen, int *mag_seen, int *u_seen, int *v_seen,
                         const psp_draw_state *st) {
    glBindTexture(GL_TEXTURE_2D, tex);
    if (*min_seen != st->tex_min_linear) {
        *min_seen = st->tex_min_linear;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, st->tex_min_linear ? GL_LINEAR : GL_NEAREST);
    }
    if (*mag_seen != st->tex_mag_linear) {
        *mag_seen = st->tex_mag_linear;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, st->tex_mag_linear ? GL_LINEAR : GL_NEAREST);
    }
    if (*u_seen != st->tex_clamp_u) {
        *u_seen = st->tex_clamp_u;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, st->tex_clamp_u ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    }
    if (*v_seen != st->tex_clamp_v) {
        *v_seen = st->tex_clamp_v;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, st->tex_clamp_v ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    }
}

/* Returns whether a texture is bound. A buffer the game drew into is used directly;
 * its coordinates are texels of the whole logical target. */
static int setup_texture(const psp_draw_state *st) {
    if (!st->tex_enable || st->clear || st->tex_w <= 0 || st->tex_h <= 0) return 0;
    if (in_vram(st->tex_addr)) {
        gl_target *t = find_target(st->tex_addr);
        if (t && t != g_bound) {
            int unknown_min = -1, unknown_mag = -1, unknown_u = -1, unknown_v = -1;
            bind_sampler(t->color, &unknown_min, &unknown_mag, &unknown_u, &unknown_v, st);
            p_glUniform2f(U.uvscale, 1.0f / (float)LOGICAL, 1.0f / (float)LOGICAL);
            return 1;
        }
    }
    gl_texture *e = get_texture(st);
    if (!e) return 0;
    bind_sampler(e->tex, &e->min_linear, &e->mag_linear, &e->clamp_u, &e->clamp_v, st);
    p_glUniform2f(U.uvscale, 1.0f / (float)st->tex_w, 1.0f / (float)st->tex_h);
    return 1;
}

/* ---- state --------------------------------------------------------------- */

static void setup_blend(const psp_draw_state *st) {
    float sfix[3] = { 1, 1, 1 };
    if (!st->blend || st->clear) {
        glDisable(GL_BLEND);
        p_glUniform3f(U.sfix, 1, 1, 1);
        return;
    }
    glEnable(GL_BLEND);

    /* Codes 6-9 are doubled alpha, which GL has no factor for. */
    GLenum src, dst;
    switch (st->blend_src) {
    case 0:  src = GL_DST_COLOR; break;
    case 1:  src = GL_ONE_MINUS_DST_COLOR; break;
    case 2: case 6:  src = GL_SRC_ALPHA; break;
    case 3: case 7:  src = GL_ONE_MINUS_SRC_ALPHA; break;
    case 4: case 8:  src = GL_DST_ALPHA; break;
    case 5: case 9:  src = GL_ONE_MINUS_DST_ALPHA; break;
    default:
        /* A fixed source factor goes in the shader: GL has only one constant,
         * and a destination factor may want it. */
        src = GL_ONE;
        sfix[0] = (float)(st->blend_sfix & 0xFF) / 255.0f;
        sfix[1] = (float)((st->blend_sfix >> 8) & 0xFF) / 255.0f;
        sfix[2] = (float)((st->blend_sfix >> 16) & 0xFF) / 255.0f;
        break;
    }
    switch (st->blend_dst) {
    case 0:  dst = GL_SRC_COLOR; break;
    case 1:  dst = GL_ONE_MINUS_SRC_COLOR; break;
    case 2: case 6:  dst = GL_SRC_ALPHA; break;
    case 3: case 7:  dst = GL_ONE_MINUS_SRC_ALPHA; break;
    case 4: case 8:  dst = GL_DST_ALPHA; break;
    case 5: case 9:  dst = GL_ONE_MINUS_DST_ALPHA; break;
    default:
        dst = GL_CONSTANT_COLOR;
        p_glBlendColor((float)(st->blend_dfix & 0xFF) / 255.0f, (float)((st->blend_dfix >> 8) & 0xFF) / 255.0f,
                       (float)((st->blend_dfix >> 16) & 0xFF) / 255.0f, 1.0f);
        break;
    }
    p_glUniform3f(U.sfix, sfix[0], sfix[1], sfix[2]);

    /* The source's alpha is written as is, as in the software path. */
    p_glBlendFuncSeparate(src, dst, GL_ONE, GL_ZERO);
    GLenum op;
    switch (st->blend_op) {
    case 1:  op = GL_FUNC_SUBTRACT; break;
    case 2:  op = GL_FUNC_REVERSE_SUBTRACT; break;
    case 3:  op = GL_MIN; break;
    case 4:  op = GL_MAX; break;
    default: op = GL_FUNC_ADD; break;      /* 5, absolute difference, has no GL form */
    }
    p_glBlendEquationSeparate(op, GL_FUNC_ADD);
}

static void setup_depth(const psp_draw_state *st) {
    static const GLenum FUNC[8] = { GL_NEVER, GL_ALWAYS, GL_EQUAL, GL_NOTEQUAL,
                                    GL_LESS, GL_LEQUAL, GL_GREATER, GL_GEQUAL };
    if (st->clear) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_ALWAYS);
        glDepthMask(st->clear_depth ? GL_TRUE : GL_FALSE);
    } else if (st->ztest) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(FUNC[st->zfunc & 7]);
        glDepthMask(st->zwrite ? GL_TRUE : GL_FALSE);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
}

static void setup_shader(const psp_draw_state *st, int textured) {
    p_glUniform1i(U.tex_on, textured);
    p_glUniform1i(U.gouraud, st->gouraud);
    if (textured) {
        p_glUniform1i(U.func, st->tex_func);
        p_glUniform1i(U.talpha, st->tex_alpha);
        p_glUniform1i(U.dbl, st->tex_double);
        p_glUniform3f(U.env, (float)(st->tex_env & 0xFF) / 255.0f, (float)((st->tex_env >> 8) & 0xFF) / 255.0f,
                      (float)((st->tex_env >> 16) & 0xFF) / 255.0f);
    }
    if (st->atest && !st->clear) {
        p_glUniform1i(U.afunc, st->atest_func);
        p_glUniform1i(U.aref, st->atest_ref);
        p_glUniform1i(U.amask, st->atest_mask);
    } else {
        p_glUniform1i(U.afunc, -1);
    }
}

/* ---- the interface ------------------------------------------------------- */

static int gl_init(int w, int h) { (void)w; (void)h; return 0; }
static void gl_shutdown(void) { }
static void gl_noop(void) { }

static void gl_target_changed(uint32_t addr, uint32_t stride, int fmt) {
    (void)stride;
    (void)fmt;
    g_target_addr = addr;
}

static gl_vertex to_gl(const psp_vertex *v) {
    gl_vertex o = { v->px, v->py, v->pz, v->pw, v->rgba, v->u, v->v };
    return o;
}

static void gl_draw(int prim, const psp_vertex *v, int count, const psp_draw_state *st) {
    if (count <= 0 || st->sc_x1 < st->sc_x0 || st->sc_y1 < st->sc_y0) return;

    bind_target();
    glEnable(GL_SCISSOR_TEST);
    glScissor(st->sc_x0 * g_scale, st->sc_y0 * g_scale, (st->sc_x1 - st->sc_x0 + 1) * g_scale,
              (st->sc_y1 - st->sc_y0 + 1) * g_scale);

    if (st->clear)
        glColorMask(st->clear_color ? GL_TRUE : GL_FALSE, st->clear_color ? GL_TRUE : GL_FALSE,
                    st->clear_color ? GL_TRUE : GL_FALSE, st->clear_alpha ? GL_TRUE : GL_FALSE);
    else
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    setup_blend(st);
    setup_depth(st);
    setup_shader(st, setup_texture(st));

    /* Targets are stored with row 0 at GL's y = 0, so the PSP's clockwise on screen
     * is counter-clockwise here. Strips alternate as GL expects. */
    if (st->cull == PSP_CULL_NONE || prim == PSP_PRIM_SPRITES) {
        glDisable(GL_CULL_FACE);
    } else {
        glEnable(GL_CULL_FACE);
        glFrontFace(st->cull == PSP_CULL_CW ? GL_CW : GL_CCW);
    }

    GLenum mode;
    int n = 0;
    switch (prim) {
    case PSP_PRIM_POINTS:         mode = GL_POINTS; break;
    case PSP_PRIM_LINES:          mode = GL_LINES; break;
    case PSP_PRIM_LINE_STRIP:     mode = GL_LINE_STRIP; break;
    case PSP_PRIM_TRIANGLE_STRIP: mode = GL_TRIANGLE_STRIP; break;
    case PSP_PRIM_TRIANGLE_FAN:   mode = GL_TRIANGLE_FAN; break;
    default:                      mode = GL_TRIANGLES; break;
    }

    if (prim == PSP_PRIM_SPRITES) {
        /* Two corners become a quad, all of it the second corner's colour and depth. */
        for (int i = 0; i + 1 < count; i += 2) {
            const gl_vertex a = to_gl(&v[i]), b = to_gl(&v[i + 1]);
            gl_vertex tl = b, tr = b, bl = b, br = b;
            tl.x = a.x; tl.y = a.y; tl.u = a.u; tl.v = a.v;
            tr.y = a.y; tr.v = a.v;
            bl.x = a.x; bl.u = a.u;
            g_vb[n++] = tl; g_vb[n++] = tr; g_vb[n++] = bl;
            g_vb[n++] = tr; g_vb[n++] = br; g_vb[n++] = bl;
        }
    } else {
        for (int i = 0; i < count; i++) g_vb[n++] = to_gl(&v[i]);
    }
    if (!n) return;

    p_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)n * sizeof(gl_vertex)), g_vb, GL_STREAM_DRAW);
    glDrawArrays(mode, 0, n);
}

/* ---- the frame rate overlay ---------------------------------------------- *
 *
 * A testing aid: frames shown per second and, under it, the slowest frame in that
 * time in milliseconds. It counts what really reaches the window. Digits are seven
 * segments of scissored clears, which needs neither a font nor a shader. */

#define OVERLAY_PERIOD_MS 500u

static void overlay_rect(int x, int y, int w, int h, float r, float g, float b) {
    glScissor(x, y, w, h);
    glClearColor(r, g, b, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

/* `top` is the digit's upper edge, in GL's upward y. */
static void overlay_digit(int x, int top, int t, int digit, float r, float g, float b) {
    static const unsigned char SEGMENTS[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };
    const unsigned s = SEGMENTS[digit];
    const int w = 4 * t;
    if (s & 0x01) overlay_rect(x, top - t, w, t, r, g, b);                   /* top */
    if (s & 0x02) overlay_rect(x + w - t, top - 4 * t, t, 4 * t, r, g, b);   /* upper right */
    if (s & 0x04) overlay_rect(x + w - t, top - 7 * t, t, 4 * t, r, g, b);   /* lower right */
    if (s & 0x08) overlay_rect(x, top - 7 * t, w, t, r, g, b);               /* bottom */
    if (s & 0x10) overlay_rect(x, top - 7 * t, t, 4 * t, r, g, b);           /* lower left */
    if (s & 0x20) overlay_rect(x, top - 4 * t, t, 4 * t, r, g, b);           /* upper left */
    if (s & 0x40) overlay_rect(x, top - 4 * t, w, t, r, g, b);               /* middle */
}

/* Three digits, right-aligned, leading zeros blank. */
static void overlay_number(int x, int top, int t, int value, float r, float g, float b) {
    if (value > 999) value = 999;
    const int d[3] = { value / 100, (value / 10) % 10, value % 10 };
    for (int i = 0; i < 3; i++) {
        if (i < 2 && !d[i] && (i == 1 ? !d[0] : 1)) continue;
        overlay_digit(x + i * 5 * t, top, t, d[i], r, g, b);
    }
}

static void draw_overlay(int image_x, int image_top, int image_h) {
    static LARGE_INTEGER freq, last_flip, period_start;
    static uint32_t frames;
    static int shown_fps, shown_worst;
    static double worst_ms;

    LARGE_INTEGER now;
    if (!freq.QuadPart) {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&period_start);
        last_flip = period_start;
    }
    QueryPerformanceCounter(&now);
    const double frame_ms = 1000.0 * (double)(now.QuadPart - last_flip.QuadPart) / (double)freq.QuadPart;
    if (frame_ms > worst_ms) worst_ms = frame_ms;
    last_flip = now;
    frames++;

    const double period_ms = 1000.0 * (double)(now.QuadPart - period_start.QuadPart) / (double)freq.QuadPart;
    if (period_ms >= OVERLAY_PERIOD_MS) {
        shown_fps = (int)((double)frames * 1000.0 / period_ms + 0.5);
        shown_worst = (int)(worst_ms + 0.5);
        frames = 0;
        worst_ms = 0;
        period_start = now;
    }

    /* The numbers are drawn in proportion to the picture, in its top left corner. */
    int t = image_h / 110;
    if (t < 2) t = 2;
    const int small = t > 3 ? t * 2 / 3 : 2;
    const int margin = t;
    const int box_w = 15 * t + 2 * margin;
    const int box_h = 7 * t + 7 * small + 3 * margin;
    glEnable(GL_SCISSOR_TEST);
    overlay_rect(image_x, image_top - box_h, box_w, box_h, 0, 0, 0);

    float r = 0.3f, g = 1.0f, b = 0.3f;                /* a full 60 */
    if (shown_fps < 45) { r = 1.0f; g = 0.3f; b = 0.3f; }
    else if (shown_fps < 57) { r = 1.0f; g = 0.9f; b = 0.2f; }
    overlay_number(image_x + margin, image_top - margin, t, shown_fps, r, g, b);
    overlay_number(image_x + margin, image_top - 2 * margin - 7 * t, small, shown_worst, 0.8f, 0.8f, 0.8f);
    glDisable(GL_SCISSOR_TEST);
}

/* ---- messages ------------------------------------------------------------ *
 *
 * One line of text over the picture for a few seconds, for what the system's own
 * screens would have said. Windows draws the text, so any language works, and the
 * result is kept as a little framebuffer to blit. */

#define MESSAGE_MS 3000u

static char g_message[256];
static volatile LONG g_message_serial;
static CRITICAL_SECTION g_message_lock;
static volatile LONG g_message_lock_ready;

void psp_render_gl_show_message(const char *utf8) {
    if (InterlockedCompareExchange(&g_message_lock_ready, 1, 0) == 0)
        InitializeCriticalSection(&g_message_lock);
    EnterCriticalSection(&g_message_lock);
    snprintf(g_message, sizeof g_message, "%s", utf8);
    LeaveCriticalSection(&g_message_lock);
    InterlockedIncrement(&g_message_serial);
}

static GLuint g_message_fbo, g_message_tex;
static int g_message_w, g_message_h, g_message_for_h;
static LONG g_message_drawn;
static ULONGLONG g_message_until;

/* Render the text with GDI into memory and hand the pixels to GL. */
static void build_message(int image_h) {
    WCHAR text[256];
    EnterCriticalSection(&g_message_lock);
    const int len = MultiByteToWideChar(CP_UTF8, 0, g_message, -1, text, 256);
    LeaveCriticalSection(&g_message_lock);
    if (len <= 1) return;

    HDC dc = CreateCompatibleDC(NULL);
    const int font_h = image_h / 18 > 12 ? image_h / 18 : 12;
    HFONT font = CreateFontA(-font_h, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                             0, "Segoe UI");
    HGDIOBJ old_font = SelectObject(dc, font);
    SIZE size;
    GetTextExtentPoint32W(dc, text, len - 1, &size);
    const int margin = font_h / 2;
    const int w = size.cx + 2 * margin, h = size.cy + margin;

    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;             /* top-down, as targets are stored */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void *bits = NULL;
    HBITMAP bitmap = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (bitmap && bits) {
        HGDIOBJ old_bitmap = SelectObject(dc, bitmap);
        memset(bits, 0, (size_t)w * (size_t)h * 4u);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        TextOutW(dc, margin, margin / 2, text, len - 1);
        GdiFlush();

        if (!g_message_fbo) g_message_fbo = make_color_target(&g_message_tex, w, h, GL_LINEAR);
        glBindTexture(GL_TEXTURE_2D, g_message_tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, bits);
        g_message_w = w;
        g_message_h = h;
        SelectObject(dc, old_bitmap);
    }
    if (bitmap) DeleteObject(bitmap);
    SelectObject(dc, old_font);
    DeleteObject(font);
    DeleteDC(dc);
}

static void draw_message(int image_x, int image_y, int image_w, int image_h) {
    const LONG serial = g_message_serial;
    if (serial != g_message_drawn) {
        g_message_drawn = serial;
        g_message_until = GetTickCount64() + MESSAGE_MS;
        g_message_for_h = 0;
    }
    if (!g_message_until || GetTickCount64() >= g_message_until) return;
    if (g_message_for_h != image_h) {
        g_message_for_h = image_h;
        build_message(image_h);
    }
    if (!g_message_fbo) return;

    const int x = image_x + (image_w - g_message_w) / 2;
    const int y = image_y + image_h / 12;
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, g_message_fbo);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);     /* creating it may have moved this */
    p_glBlitFramebuffer(0, 0, g_message_w, g_message_h, x, y + g_message_h, x + g_message_w, y,
                        GL_COLOR_BUFFER_BIT, GL_NEAREST);
}

static void keep_snapshot(GLuint source_fbo, int source_w, int source_h) {
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, source_fbo);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_small_fbo);
    p_glBlitFramebuffer(0, 0, source_w, source_h, 0, 0, PSP_W, PSP_H, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, g_small_fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, PSP_W, PSP_H, GL_RGBA, GL_UNSIGNED_BYTE, g_snapshot);
    g_have_snapshot = 1;
}

/* Show the buffer at `addr`. The game may have filled it with the CPU rather than
 * by drawing, and then it is only in guest memory. */
static void gl_flip(uint32_t addr, uint32_t stride, int fmt) {
    static uint32_t presents;
    GLuint source;
    int source_w, source_h;
    GLenum filter;

    gl_target *t = find_target(addr);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    if (t) {
        source = t->fbo;
        source_w = PSP_W * g_scale;
        source_h = PSP_H * g_scale;
        filter = GL_LINEAR;
    } else {
        const uint32_t bytes = fmt == 3 ? 4u : 2u;
        for (uint32_t y = 0; y < PSP_H; y++) {
            for (uint32_t x = 0; x < PSP_W; x++) {
                const uint32_t at = addr + (y * stride + x) * bytes;
                const uint32_t px = bytes == 4 ? psp_fast_read32(at) : psp_fast_read16(at);
                g_upload[y * PSP_W + x] = psp_color_expand(px, fmt);
            }
        }
        glBindTexture(GL_TEXTURE_2D, g_vram_tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, PSP_W, PSP_H, GL_RGBA, GL_UNSIGNED_BYTE, g_upload);
        source = g_vram_fbo;
        source_w = PSP_W;
        source_h = PSP_H;
        filter = GL_NEAREST;
    }

    if (++presents % SNAPSHOT_EVERY == 0) keep_snapshot(source, source_w, source_h);

    /* Fit the window, keeping the PSP's shape. Rows are stored top first, and the
     * window's y runs upwards, hence the swapped destination. */
    RECT r;
    GetClientRect(WindowFromDC(g_dc), &r);
    const int cw = r.right, ch = r.bottom;
    int w = cw, h = cw * PSP_H / PSP_W;
    if (h > ch) { h = ch; w = ch * PSP_W / PSP_H; }
    const int x0 = (cw - w) / 2, y0 = (ch - h) / 2;

    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, source);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glViewport(0, 0, cw, ch);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    p_glBlitFramebuffer(0, 0, source_w, source_h, x0, y0 + h, x0 + w, y0, GL_COLOR_BUFFER_BIT, filter);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    draw_message(x0, y0, w, h);
    if (g_overlay) draw_overlay(x0, y0 + h, h);
    SwapBuffers(g_dc);

    g_bound = NULL;
    g_frame++;
}

static int gl_read_frame(uint32_t *out) {
    if (!g_have_snapshot) return -1;
    memcpy(out, g_snapshot, sizeof g_snapshot);
    return 0;
}

const psp_render_backend psp_render_gl = {
    "gl", gl_init, gl_shutdown, gl_target_changed, gl_draw, gl_noop, gl_noop,
    gl_thread_init, gl_flip, gl_read_frame
};
