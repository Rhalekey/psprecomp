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
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Targets are all this size in guest pixels, whatever the game's stride. */
#define LOGICAL 512
#define PSP_W   480
#define PSP_H   272

/* With the wide canvas on, everything is drawn in the middle of a larger target, so that what
 * the 3D scene has past the PSP's screen lands somewhere. These are what shapes it can reach. */
#define WIDE_CANVAS_W 976     /* a little past 32:9 at the PSP's height */
#define WIDE_CANVAS_H 512
/* In a taller window, flat pieces wholly above the first line or below the second go with the
 * window's top or bottom. What is parked off the screen goes with them, out of sight. A piece as
 * wide as the screen is a band, and goes by which part of the screen its middle is in. */
#define PIN_TOP         50.0f
#define PIN_BOTTOM      240.0f
/* A song's meter, in its corner, reaches higher than a list's last line, which is further right. */
#define METER_RIGHT     60.0f
/* Just under the 218 a song's band starts at, as its position comes out a little under. */
#define METER_TOP       216.0f
#define METER_BOTTOM    235.0f
/* The results' words about the song, as NOT CLEAR, sit just above that screen's band. */
#define RESULT_WORDS_TOP 185.0f
/* In a wider window, what ends at the screen's right edge and is this wide runs on to the window's. */
#define SIDE_BAND_WIDTH 200.0f
#define BOTTOM_LEFT_END 100.0f
/* Only a band's thin pieces take up the room at its end themselves. */
#define THIN_BAND_HEIGHT 40.0f
#define DIALOG_WIDTH    150.0f
#define DIALOG_OFF_MIDDLE 30.0f
#define BAND_WIDTH      470.0f
#define BAND_TOP        68.0f
#define BAND_BOTTOM     204.0f
/* A flat piece's w is 1 but for rounding, and it covers the screen give or take a little. */
#define FLAT_W_NEAR     0.01f
#define COVERS_NEAR     2.0f
/* The picture a song's combo is drawn from. */
#define COMBO_W         127.0f
/* Pieces of the combo reach a row or two past their places on the sheet's picture. */
#define ROWS_NEAR       2.0f
#define COMBO_H         64.0f
/* A covering piece showing fewer texels than this across is one texel or so stretched: a veil. */
#define PICTURE_SPAN    64.0f

#define MAX_TARGETS   16
#define TEX_SLOTS     2048
#define TEX_MAX_LIVE  1024
#define TEX_MAX_SIDE  1024
#define HASH_SAMPLES  4096       /* words of a texture checked for changes per frame */
#define SNAPSHOT_WAIT_MS 500     /* how long a capture waits for a present */

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
    X(PFNGLUNIFORM1FPROC, glUniform1f) \
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
    X(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers) \
    X(PFNGLDELETERENDERBUFFERSPROC, glDeleteRenderbuffers) \
    X(PFNGLRENDERBUFFERSTORAGEPROC, glRenderbufferStorage) \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus) \
    X(PFNGLBLITFRAMEBUFFERPROC, glBlitFramebuffer) \
    X(PFNGLGENERATEMIPMAPPROC, glGenerateMipmap) \
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
    "uniform vec2 u_uvat;\n"
    "uniform vec2 u_canvas_at;\n"
    "uniform vec2 u_canvas_half;\n"
    "uniform float u_zoom;\n"
    "out vec4 v_col;\n"
    "flat out vec4 v_flat;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    /* Pixels (times w) to clip space. Depth 0..65535 becomes the window's 0..1. */
    "    float w = a_pos.w;\n"
    /* The 3D scene can be enlarged about the middle of the screen. What is drawn in screen
     * coordinates has a w of exactly 1, and stays. */
    "    float zoom = abs(w - 1.0) < 0.01 ? 1.0 : u_zoom;\n"
    "    float x = (a_pos.x - 240.0 * w) * zoom + (240.0 + u_canvas_at.x) * w;\n"
    "    float y = (a_pos.y - 136.0 * w) * zoom + (136.0 + u_canvas_at.y) * w;\n"
    "    gl_Position = vec4(x / u_canvas_half.x - w, y / u_canvas_half.y - w, a_pos.z / 32767.5 - w, w);\n"
    "    v_col = a_col;\n"
    "    v_flat = a_col;\n"
    "    v_uv = a_uv * u_uvscale + u_uvat;\n"
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
    "uniform float u_blur;\n"
    "uniform float u_blur_lod;\n"
    "out vec4 o_col;\n"
    /* Samples spread over a disc, for the copy behind a picture that does not fill the window.
     * The disc turns from pixel to pixel, which leaves fine grain where a fixed one leaves rings. */
    "vec4 look(vec2 uv) {\n"
    "    if (u_blur <= 0.0) return texture(u_tex, uv);\n"
    "    if (u_blur_lod > 0.0) {\n"
    "        vec2 d = vec2(u_blur * 0.5);\n"
    "        return (textureLod(u_tex, uv, u_blur_lod) + textureLod(u_tex, uv + vec2(d.x, d.y), u_blur_lod) +\n"
    "                textureLod(u_tex, uv + vec2(-d.x, d.y), u_blur_lod) + textureLod(u_tex, uv + vec2(d.x, -d.y), u_blur_lod) +\n"
    "                textureLod(u_tex, uv - d, u_blur_lod)) / 5.0;\n"
    "    }\n"
    "    float turn = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453) * 6.283185;\n"
    "    vec4 sum = texture(u_tex, uv);\n"
    "    for (int i = 0; i < 48; i++) {\n"
    "        float angle = float(i) * 2.399963 + turn;\n"
    "        float r = u_blur * sqrt((float(i) + 1.0) / 48.0);\n"
    "        sum += texture(u_tex, uv + r * vec2(cos(angle), sin(angle)));\n"
    "    }\n"
    "    return sum / 49.0;\n"
    "}\n"
    "void main() {\n"
    "    vec4 c = u_gouraud != 0 ? v_col : v_flat;\n"
    "    float fade = c.a;\n"
    "    if (u_tex_on != 0) {\n"
    "        vec4 t = look(v_uv);\n"
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
    "    o_col = vec4(c.rgb * u_sfix, u_blur > 0.0 ? fade : c.a);\n"
    "}\n";

typedef struct { float x, y, z, w; uint32_t rgba; float u, v; } gl_vertex;

typedef struct {
    int used;
    uint32_t addr;
    GLuint fbo, color, depth;
    uint64_t last_used;
    int shown;       /* has been on screen, so is the game's image and not a texture being made */
} gl_target;

typedef struct {
    int used;
    uint32_t addr;
    int w, h, fmt, bw, swizzle;
    uint32_t clut_hash, data_hash;
    uint64_t checked_frame;
    GLuint tex;
    int min_linear, mag_linear, clamp_u, clamp_v;
    int replaced;    /* a larger file stands in for it */
    int up_w, up_h;  /* its size on the GPU */
    int mipped;      /* its smaller levels are made, for a soft blur */
    int glyphs;      /* its file gave a shape only, as a font's letters: no smaller levels for it */
    uint32_t colours;  /* what it is known by, or 0 if that was not worked out */
    int use_logged;
} gl_texture;

static struct {
    GLint tex_on, func, talpha, dbl, gouraud, afunc, aref, amask, env, sfix, uvscale, uvat, canvas_at, canvas_half, zoom, blur, blur_lod;
} U;

static HDC    g_dc;
static int    g_scale = 2;
static volatile int g_scale_asked = 2;
static int    g_scale_taken_up;
static int    g_wide;
/* 0: the PSP's screen with bars around it. 1: a window taller than that screen enlarges the 3D
 * scene to its height, cutting the scene's sides. 2: it shows more of the scene above and below. */
static volatile int g_wide_view = 2;
/* A flat picture covers the screen, this frame or the last: what is on it stays on it. */
static int    g_flat_now, g_flat_last;
/* A card's ground and its other pieces come from one sheet, while an illustration is alone on
 * its own. Known from the frame before, so an illustration is never stretched. */
static uint32_t g_flat_sheet;
static int    g_card_now, g_card_last;
/* The results' band is on show, this frame or the last. */
static int    g_results_now, g_results_last;
/* The dimming behind a dialog was drawn this frame. */
static int    g_veil_now;
/* A window in the middle of the screen is on show, this frame or the last: what is in it stays. */
static int    g_dialog_now, g_dialog_last;
/* A message window is sized by the game for its Japanese, before its lines are known. A wider
 * translated line is told here, and the window's frame is drawn wider to hold it: its pieces
 * left of the middle move left, those right of it move right, and those across it stretch. Its
 * edges and the widest line are known from the frame before, as the frame is drawn first. */
static float  g_dialog_line_now, g_dialog_line_last;
static float  g_frame_left_now = 1e9f, g_frame_right_now = -1e9f, g_frame_left_last = 1e9f, g_frame_right_last = -1e9f;
static float  g_frame_top_now = 1e9f, g_frame_bottom_now = -1e9f, g_frame_top_last = 1e9f, g_frame_bottom_last = -1e9f;
#define FRAME_BAR_NEAR 8.0f      /* the teal bars sit this close above or below the frame */
#define FRAME_GROUND_SHARE 0.8f  /* and the ground is at least this much of its width */
#define DIALOG_TEXT_MARGINS 32.0f   /* the game's own: 16 on each side of the text */
static float  g_zoom = 1.0f;
/* How much of the canvas the window shows, in guest pixels, as of the last frame. */
static int    g_show_w = PSP_W, g_show_h = PSP_H;
static int    g_canvas_w = LOGICAL, g_canvas_h = LOGICAL;
static int    g_canvas_x, g_canvas_y;
static int    g_canvas_told;
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
/* Made only when asked for: a copy waits for the GPU to finish, which can cost a whole frame. */
static volatile LONG g_snapshot_asked;
static volatile int g_have_snapshot;

/* A song's scene is drawn first, then its notes with no depth test, then its flat HUD, so a veil
 * goes in before the first of those two. It is on for a few frames after the host last said a
 * song is on, as its word may come after the frame has begun. */
#define SONG_FRAMES 3
static volatile int g_dim_percent;
static volatile int g_song_frames;
static int g_dim_done;

void psp_render_gl_set_background_dim(int percent) { g_dim_percent = percent; }
void psp_render_gl_song_frame(void) { g_song_frames = SONG_FRAMES; }

static int g_overlay;
static int g_present_linear = 1;

void psp_render_gl_set_present_filter(int linear) { g_present_linear = linear; }

/* Larger versions of whole textures, made outside the game, and named for a hash of the
 * original that survives the way the game stores colours. An .rgba file replaces a picture.
 * An .alpha file holds one byte of coverage over white per pixel, for a font. */
#define REPLACE_MIN_SIDE 32
#define REPLACE_MAX_SIDE 4096
#define REPLACE_SOLID_ALPHA 127

static char g_replace_dir[1024];
static char g_dump_dir[1024];

/* Pictures in the language being played, tried first. Asked for from any thread, and taken
 * up by the one that draws, which then has to forget what it has made. */
static char g_language_dir[1024];
static void *volatile g_language_asked;

/* Pictures drawn from vectors, tried before those: one folder, or two with | between them.
 * Asked for in the same way. With two, the second's swap_regions.txt lists pictures that have
 * two regions change places. */
static char g_vector_dir[1024];
static void *volatile g_vector_asked;

/* Notes and targets in another style, tried before all of those. Its targets.rgba is drawn in
 * place of the game's targets, whose shapes come from their models (see note_target_drawn). */
static char g_note_dir[1024];
static void *volatile g_note_asked;

void psp_render_gl_set_dump_dir(const char *dir) {
    snprintf(g_dump_dir, sizeof g_dump_dir, "%s", dir ? dir : "");
}

void psp_render_gl_set_replacement_dir(const char *dir) {
    snprintf(g_replace_dir, sizeof g_replace_dir, "%s", dir ? dir : "");
}

/* Only ever replaced whole, so that the thread that draws never reads half of one. */
static void ask_for_dir(void *volatile *asked, const char *dir) {
    const char *text = dir ? dir : "";
    char *copy = (char *)malloc(strlen(text) + 1);
    if (!copy) return;
    strcpy(copy, text);
    free(InterlockedExchangePointer((void *volatile *)asked, copy));
}

void psp_render_gl_set_language_dir(const char *dir) { ask_for_dir(&g_language_asked, dir); }
void psp_render_gl_set_vector_dir(const char *dir) { ask_for_dir(&g_vector_asked, dir); }
void psp_render_gl_set_note_dir(const char *dir) { ask_for_dir(&g_note_asked, dir); }

void psp_render_gl_set_overlay(int on) { g_overlay = on; }

void psp_render_gl_dialog_line(float width) {
    if (width > g_dialog_line_now) g_dialog_line_now = width;
}

/* The pieces of a message window's frame, by where they are on their sheets, both 256 square:
 * the frame with its corner marks and its ground on the interface's sheet, and the teal bars
 * above and below it on the glow sheet. */
enum { NOT_FRAME, FRAME_EDGE, FRAME_FILL };
/* A message window comes after the dimming that covers the screen, and is drawn from the interface's
 * 256 square sheet: its frame with the corner marks from the columns 150 to 210, which give the
 * window's edges, and its ground and teal bars from the rest. Words and button signs are on other
 * sheets, and stay where they are. */
static int frame_piece(const psp_draw_state *st, float u_low, float u_high, float v_low, float v_high) {
    (void)v_low;
    (void)v_high;
    if (!g_veil_now || !st->tex_enable || st->tex_w != 256 || st->tex_h != 256) return NOT_FRAME;
    return u_low >= 150.0f && u_high <= 210.0f ? FRAME_EDGE : FRAME_FILL;
}

/* Widens a frame piece just put in g_vb for the line it has to hold. The frame's own pieces and
 * the bars above and below it go out, and the ground stretches; buttons and the like inside the
 * window, which come from the same sheet, stay where the game put them. */
static void widen_frame_piece(int kind, int n, float left, float right, float top, float bottom) {
    if (kind == FRAME_EDGE) {
        if (left < g_frame_left_now) g_frame_left_now = left;
        if (right > g_frame_right_now) g_frame_right_now = right;
        if (top < g_frame_top_now) g_frame_top_now = top;
        if (bottom > g_frame_bottom_now) g_frame_bottom_now = bottom;
    } else {
        const int bar = bottom <= g_frame_top_last + FRAME_BAR_NEAR || top >= g_frame_bottom_last - FRAME_BAR_NEAR;
        const int ground = right - left >= (g_frame_right_last - g_frame_left_last) * FRAME_GROUND_SHARE;
        if (!bar && !ground) return;
    }
    if (g_dialog_line_last <= 0.0f || g_frame_right_last <= g_frame_left_last) return;
    const float grow = g_dialog_line_last + DIALOG_TEXT_MARGINS - (g_frame_right_last - g_frame_left_last);
    if (grow <= 0.0f) return;
    const float middle = (g_frame_left_last + g_frame_right_last) / 2.0f;
    const int across = left < middle - 0.5f && right > middle + 0.5f;
    for (int i = 0; i < n; i++) {
        const int on_left = across ? g_vb[i].x < middle : right <= middle + 0.5f;
        g_vb[i].x += on_left ? -grow / 2.0f : grow / 2.0f;
    }
}

/* ---- the footer, laid out for its words ----------------------------------- *
 *
 * A menu's footer is a band with the buttons' hints on it, each a piece of fixed width on one sheet,
 * whose words had to fit those widths. pieces.txt, beside the pictures drawn from vectors, names
 * that sheet by its colours and lists each hint's place on it with the width its words really
 * take, and a place in the sheet's spare rows for one that did not fit:
 *
 *     sheet <colours in hex>
 *     hint <u> <v> <w> <h> <new u> <new v> <new w> <new h>
 *     band <u> <v> <w> <h>
 *     extra <number> <u> <v> <w> <h>
 *     extra_with <number> <u> <v>
 *
 * An extra is a hint the game does not have, for something added to it: while a screen asks for it
 * (psp_render_gl_footer_extras), or while the game's hint at <u> <v> on the sheet is, it is drawn
 * before the game's first hint, as one more of them.
 * The hints at the bottom of the screen are put side by side at those widths, ending where the last
 * one did, and the band starts FOOTER_BAND_LEAD before the first, so that it always just holds them.
 * Only screens with the band are laid out again, so the editor keeps its own layout.
 *
 * The game draws the band before the hints, and while the hints change it draws a frame or two with
 * some of them missing, or moves the band. So nothing is worked out from the frame being drawn:
 * each place the game draws a hint or a piece of the band at is given a place of its own, learnt
 * once a set of hints has stayed the same for FOOTER_STEADY frames, and the same frame of the game
 * always comes out the same. A band piece at a place not learnt yet moves as the band last did. */
#define FOOTER_PIECES 160
#define FOOTER_HINTS 24
#define FOOTER_BANDS 4
#define FOOTER_PLACES 160
#define FOOTER_TOP 230.0f       /* hints this low on the screen are the footer's */
#define FOOTER_GAP 10.0f        /* between one hint's words and the next one's sign */
#define FOOTER_BAND_LEAD 32.0f  /* the band's slanted end starts this far before the first hint, as in the game */
#define FOOTER_EDGE 6.0f        /* the last hint's words end at least this far from the window's edge */
#define FOOTER_UV_NEAR 1.0f
#define FOOTER_SAME 0.5f
#define FOOTER_STEADY 3

typedef struct {
    float u, v, w, h, nu, nv, nw, nh;
    int band;
    int extra;   /* its number and one, for an extra hint; it is only at nu, nv, nw, nh */
} footer_piece;

#define FOOTER_EXTRAS 8
#define FOOTER_EXTRA_LEFT -1000.0f   /* where an extra counts as drawn, before any of the game's */
static volatile unsigned g_footer_extras_asked;
static volatile uint64_t g_footer_extras_frame;
static uint64_t g_footer_extras_done;
static float g_footer_hint_scale = 1.0f;   /* of the game's last hint drawn */
static int g_footer_hint_piece = -1;       /* the same, and where the game drew it */
static float g_footer_hint_left;
/* How far each extra's place was from a hint of the game's, once learnt, to follow it as it slides. */
static int g_footer_extra_from[FOOTER_EXTRAS];
static float g_footer_extra_dx[FOOTER_EXTRAS];
/* The game's hint each extra comes with, by its place on the sheet, if any; and those that came last frame. */
static int g_footer_extra_with[FOOTER_EXTRAS];
static float g_footer_extra_with_u[FOOTER_EXTRAS], g_footer_extra_with_v[FOOTER_EXTRAS];
static unsigned g_footer_extras_with;
void psp_render_gl_footer_extras(unsigned mask) {
    g_footer_extras_asked = mask;
    g_footer_extras_frame = g_frame;
}

/* A piece drawn at `left` by the game, and where it goes, with the other hints drawn with it: the
 * same hint at the same place goes elsewhere when another is beside it, as on a screen whose hints
 * change with the row chosen. */
typedef struct {
    int piece;
    float left, to;
    uint32_t set;
} footer_place;

static footer_piece g_footer_pieces[FOOTER_PIECES];
static int g_footer_piece_count;
static uint32_t g_footer_sheet;
static char g_footer_read_for[1024] = "\x01";
static uint32_t g_tex_colours;   /* the bound texture's, set by setup_texture */
static gl_texture *g_tex_bound;  /* the same, or NULL for a target or none */

#define IN_PLACE_SHEETS 8
static uint32_t g_in_place[IN_PLACE_SHEETS];
static int g_in_place_n;
/* Such a sheet was drawn this frame or the last: its scene is left as the game made it. */
static int g_in_place_now, g_in_place_last;
void psp_render_gl_keep_in_place(uint32_t colours) {
    if (g_in_place_n < IN_PLACE_SHEETS) g_in_place[g_in_place_n++] = colours;
}

/* What this frame drew, and what has been learnt. */
static footer_place g_footer_hints_now[FOOTER_HINTS], g_footer_bands_now[FOOTER_BANDS];
static int g_footer_hints_n, g_footer_bands_n, g_footer_band_now;
static footer_place g_footer_places[FOOTER_PLACES];
static int g_footer_places_n, g_footer_places_next;
static float g_footer_shift;   /* the band's last move, for a place not learnt yet */
/* Where the band is shown while it slides, how far that is behind where it goes, and the frame of each. */
#define FOOTER_SLIDE 0.5f      /* of the way left, each frame */
static float g_footer_shown, g_footer_lag;
static uint64_t g_footer_shown_frame, g_footer_slide_frame;

static void footer_places_load(void);

static void read_footer_pieces(void) {
    if (!strcmp(g_footer_read_for, g_vector_dir)) return;
    snprintf(g_footer_read_for, sizeof g_footer_read_for, "%s", g_vector_dir);
    g_footer_piece_count = 0;
    g_footer_sheet = 0;
    g_footer_places_n = 0;
    g_footer_shift = 0.0f;
    memset(g_footer_extra_with, 0, sizeof g_footer_extra_with);
    g_footer_extras_with = 0;
    if (!g_vector_dir[0]) return;
    const char *second = strchr(g_vector_dir, '|');
    const int first_len = second ? (int)(second - g_vector_dir) : (int)strlen(g_vector_dir);
    char path[1200];
    snprintf(path, sizeof path, "%.*s\\pieces.txt", first_len, g_vector_dir);
    FILE *f = fopen(path, "r");
    if (!f && second) {
        snprintf(path, sizeof path, "%s\\pieces.txt", second + 1);
        f = fopen(path, "r");
    }
    if (!f) return;
    char line[160];
    while (fgets(line, sizeof line, f)) {
        footer_piece p;
        unsigned sheet;
        memset(&p, 0, sizeof p);
        if (sscanf(line, "sheet %x", &sheet) == 1) {
            g_footer_sheet = sheet;
        } else if (g_footer_piece_count < FOOTER_PIECES &&
                   sscanf(line, "hint %f %f %f %f %f %f %f %f", &p.u, &p.v, &p.w, &p.h, &p.nu, &p.nv, &p.nw, &p.nh) == 8) {
            g_footer_pieces[g_footer_piece_count++] = p;
        } else if (sscanf(line, "extra_with %d %f %f", &p.extra, &p.u, &p.v) == 3) {
            if (p.extra >= 0 && p.extra < FOOTER_EXTRAS) {
                g_footer_extra_with[p.extra] = 1;
                g_footer_extra_with_u[p.extra] = p.u;
                g_footer_extra_with_v[p.extra] = p.v;
            }
        } else if (g_footer_piece_count < FOOTER_PIECES &&
                   sscanf(line, "extra %d %f %f %f %f", &p.extra, &p.nu, &p.nv, &p.nw, &p.nh) == 5) {
            p.extra++;
            p.u = p.v = -1e6f;   /* never matches what the game draws */
            g_footer_pieces[g_footer_piece_count++] = p;
        } else if (g_footer_piece_count < FOOTER_PIECES && sscanf(line, "band %f %f %f %f", &p.u, &p.v, &p.w, &p.h) == 4) {
            p.band = 1;
            g_footer_pieces[g_footer_piece_count++] = p;
        }
    }
    fclose(f);
    fprintf(stderr, "gl: footer pieces from %s: %d, sheet %08X\n", path, g_footer_piece_count, g_footer_sheet);
    footer_places_load();
}

static uint32_t g_footer_set;   /* the hints of the last frame, as footer_set_of makes it */

static footer_place *footer_place_in(int piece, float left, uint32_t set) {
    for (int i = 0; i < g_footer_places_n; i++) {
        if (g_footer_places[i].piece == piece && fabsf(g_footer_places[i].left - left) < FOOTER_SAME && g_footer_places[i].set == set)
            return &g_footer_places[i];
    }
    return NULL;
}

/* Where a piece goes with the hints of the last frame, or, while they change, with any others. */
static footer_place *footer_place_of(int piece, float left) {
    footer_place *found = footer_place_in(piece, left, g_footer_set);
    for (int i = 0; i < g_footer_places_n && !found; i++) {
        if (g_footer_places[i].piece == piece && fabsf(g_footer_places[i].left - left) < FOOTER_SAME) found = &g_footer_places[i];
    }
    return found;
}

static uint32_t footer_set_of(const footer_place *h, int n) {
    uint32_t set = 2166136261u;
    for (int i = 0; i < n; i++) {
        set = (set ^ (uint32_t)h[i].piece) * 16777619u;
        set = (set ^ (uint32_t)(int32_t)lrintf(h[i].left)) * 16777619u;
    }
    return set;
}

/* What has been learnt is kept in a file, footer_places.txt in the folder psp_render_gl_set_state_dir
 * names, so that a screen seen once, in a game before, comes out right from its first frame: its band
 * slides in to its place instead of jumping there once the game's slide has stopped. Each line is
 *     <pictures> <window width> <piece> <left> <to> <set>
 * where <pictures> stands for the folder of pictures and its pieces, whose numbering it goes by. */
#define FOOTER_STORE 2048
typedef struct {
    uint32_t pictures;
    int w;
    footer_place place;
} footer_stored;
static footer_stored g_footer_store[FOOTER_STORE];
static int g_footer_store_n, g_footer_store_read, g_footer_store_dirty;
static char g_state_dir[1024];

void psp_render_gl_set_state_dir(const char *dir) { snprintf(g_state_dir, sizeof g_state_dir, "%s", dir ? dir : ""); }

static uint32_t footer_pictures(void) {
    uint32_t key = 2166136261u;
    for (const char *c = g_vector_dir; *c; c++) key = (key ^ (uint8_t)*c) * 16777619u;
    return (key ^ (uint32_t)g_footer_piece_count) * 16777619u;
}

static void footer_store_path(char *path, size_t size) { snprintf(path, size, "%s\\footer_places.txt", g_state_dir); }

static void footer_store_read(void) {
    if (g_footer_store_read || !g_state_dir[0]) return;
    g_footer_store_read = 1;
    char path[1100], line[160];
    footer_store_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f) && g_footer_store_n < FOOTER_STORE) {
        footer_stored e;
        unsigned pictures, set;
        if (sscanf(line, "%x %d %d %f %f %x", &pictures, &e.w, &e.place.piece, &e.place.left, &e.place.to, &set) != 6) continue;
        e.pictures = pictures;
        e.place.set = set;
        g_footer_store[g_footer_store_n++] = e;
    }
    fclose(f);
}

static void footer_store_write(void) {
    g_footer_store_dirty = 0;
    if (!g_state_dir[0]) return;
    char path[1100];
    footer_store_path(path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < g_footer_store_n; i++) {
        const footer_stored *e = &g_footer_store[i];
        fprintf(f, "%08X %d %d %.2f %.2f %08X\n", e->pictures, e->w, e->place.piece, e->place.left, e->place.to, e->place.set);
    }
    fclose(f);
}

static void footer_store_put(const footer_place *place) {
    footer_store_read();
    const uint32_t pictures = footer_pictures();
    for (int i = 0; i < g_footer_store_n; i++) {
        footer_stored *e = &g_footer_store[i];
        if (e->pictures == pictures && e->w == g_show_w && e->place.piece == place->piece && e->place.set == place->set &&
            fabsf(e->place.left - place->left) < FOOTER_SAME) {
            if (fabsf(e->place.to - place->to) >= 0.01f) g_footer_store_dirty = 1;
            e->place.to = place->to;
            return;
        }
    }
    /* Full, the oldest goes. */
    if (g_footer_store_n == FOOTER_STORE) {
        memmove(g_footer_store, g_footer_store + 1, sizeof g_footer_store[0] * (FOOTER_STORE - 1));
        g_footer_store_n--;
    }
    g_footer_store[g_footer_store_n++] = (footer_stored){pictures, g_show_w, *place};
    g_footer_store_dirty = 1;
}

/* The places kept for these pictures and this width of window, the latest last. */
static void footer_places_load(void) {
    footer_store_read();
    g_footer_places_n = 0;
    g_footer_places_next = 0;
    const uint32_t pictures = footer_pictures();
    for (int i = 0; i < g_footer_store_n; i++) {
        const footer_stored *e = &g_footer_store[i];
        if (e->pictures != pictures || e->w != g_show_w) continue;
        g_footer_places[g_footer_places_next] = e->place;
        g_footer_places_next = (g_footer_places_next + 1) % FOOTER_PLACES;
        if (g_footer_places_n < FOOTER_PLACES) g_footer_places_n++;
    }
}

static void footer_learn(int piece, float left, float to) {
    footer_place *known = footer_place_in(piece, left, g_footer_set);
    if (!known) {
        /* The oldest makes room. */
        known = &g_footer_places[g_footer_places_next];
        g_footer_places_next = (g_footer_places_next + 1) % FOOTER_PLACES;
        if (g_footer_places_n < FOOTER_PLACES) g_footer_places_n++;
    }
    *known = (footer_place){piece, left, to, g_footer_set};
    footer_store_put(known);
}

/* Six vertices more at the end of g_vb: the quad `q` from its edge at `from` on to `to`, the edge's
 * column repeated. Gives back how many vertices there are now. */
static int continue_quad(const gl_vertex *q, int n, float from, float to, float u_at_edge) {
    if (n + 6 > (int)(sizeof g_vb / sizeof g_vb[0])) return n;
    /* The edge's own column, half a texel in: on the edge itself, filtering mixes in what is past it,
     * often clear, or the other side of a texture that wraps, and the band's continuation comes out
     * faint or of another colour. */
    float u_inside = u_at_edge;
    for (int i = 0; i < 6; i++) {
        if (fabsf(q[i].x - from) >= 0.5f) u_inside = q[i].u;
    }
    if (u_inside != u_at_edge) u_at_edge += u_inside > u_at_edge ? 0.5f : -0.5f;
    for (int i = 0; i < 6; i++) {
        gl_vertex more = q[i];
        more.x = fabsf(q[i].x - from) < 0.5f ? to : from;
        more.u = u_at_edge;
        g_vb[n + i] = more;
    }
    return n + 6;
}

/* A band piece just moved by `shift`: in a wide window its right end goes on to the window's edge.
 * Gives back how many vertices there are now. */
static int footer_band_on(const gl_vertex *q, float right, float shift, int n) {
    const float edge = (float)(g_canvas_w - g_canvas_x), from = right + shift;
    if (!g_wide || from >= edge) return n;
    float u_edge = q[0].u;
    for (int i = 0; i < 6; i++) {
        if (fabsf(q[i].x - from) < FOOTER_SAME) u_edge = q[i].u;
    }
    return continue_quad(q, n, from, edge, u_edge);
}

/* A quad just put in g_vb from the footer's sheet. `sheet_h` turns the list's rows, counted from the
 * top, into the game's, counted from the bottom. Gives back how many vertices there are now. */
static int footer_piece_drawn(gl_vertex *q, float sheet_h, int n, int *handled) {
    float left = q[0].x, right = left, top = q[0].y, bottom = top;
    float u_low = q[0].u, u_high = u_low, v_low = q[0].v, v_high = v_low;
    for (int i = 1; i < 6; i++) {
        left = fminf(left, q[i].x);
        right = fmaxf(right, q[i].x);
        top = fminf(top, q[i].y);
        bottom = fmaxf(bottom, q[i].y);
        u_low = fminf(u_low, q[i].u);
        u_high = fmaxf(u_high, q[i].u);
        v_low = fminf(v_low, q[i].v);
        v_high = fmaxf(v_high, q[i].v);
    }
    int found = -1;
    for (int i = 0; i < g_footer_piece_count && found < 0; i++) {
        const footer_piece *p = &g_footer_pieces[i];
        if (fabsf(u_low - p->u) <= FOOTER_UV_NEAR && fabsf(u_high - (p->u + p->w)) <= FOOTER_UV_NEAR &&
            fabsf(v_low - (sheet_h - p->v - p->h)) <= FOOTER_UV_NEAR && fabsf(v_high - (sheet_h - p->v)) <= FOOTER_UV_NEAR)
            found = i;
    }
    if (found < 0) return n;
    const footer_piece *p = &g_footer_pieces[found];
    const footer_place *place = footer_place_of(found, left);

    /* In a taller window the footer goes to its bottom. */
    if (g_wide && g_show_h > PSP_H && (p->band || top >= FOOTER_TOP)) {
        const float down = (float)(g_show_h - PSP_H) / 2.0f;
        for (int i = 0; i < 6; i++) q[i].y += down;
    }

    if (p->band) {
        *handled = 1;
        const int main_band = p->w >= 500.0f && p->h >= 20.0f;
        if (main_band) g_footer_band_now = 1;
        /* What runs from the screen's left edge stays, and in a wide window goes on to both edges. */
        if (left <= 1.0f) {
            if (!g_wide) return n;
            const gl_vertex at[6] = {q[0], q[1], q[2], q[3], q[4], q[5]};
            float u_left = q[0].u, u_right = q[0].u;
            for (int i = 0; i < 6; i++) {
                if (fabsf(q[i].x - left) < FOOTER_SAME) u_left = q[i].u;
                if (fabsf(q[i].x - right) < FOOTER_SAME) u_right = q[i].u;
            }
            n = continue_quad(at, n, left, (float)-g_canvas_x, u_left);
            if (right >= (float)PSP_W - 1.0f) n = continue_quad(at, n, right, (float)(g_canvas_w - g_canvas_x), u_right);
            return n;
        }
        if (g_footer_bands_n < FOOTER_BANDS) g_footer_bands_now[g_footer_bands_n++] = (footer_place){found, left, left, 0};
        /* At a place not learnt, as while the game slides the band in under hints that have stopped,
         * it moves as it did with these hints, if that was learnt, so that it slides in to where it
         * stays; else as the band last did. */
        float shift = g_footer_shift;
        if (place) {
            shift = place->to - left;
        } else {
            for (int i = 0; i < g_footer_places_n; i++) {
                const footer_place *known = &g_footer_places[i];
                if (known->piece == found && known->set == g_footer_set) shift = known->to - known->left;
            }
        }
        if (place && main_band) g_footer_shift = shift;
        /* The band slides to a new place rather than jumping, over a few frames: a frame the game draws
         * out of step while its hints change becomes part of the slide. Its line art goes with it. */
        if (main_band) {
            const float target = left + shift;
            if (g_footer_slide_frame != g_frame) {
                g_footer_slide_frame = g_frame;
                g_footer_shown = g_footer_shown_frame + 1 == g_frame && fabsf(target - g_footer_shown) > FOOTER_SAME
                                     ? g_footer_shown + (target - g_footer_shown) * FOOTER_SLIDE
                                     : target;
                g_footer_lag = g_footer_shown - target;
            }
            g_footer_shown_frame = g_frame;
        }
        if (g_footer_shown_frame == g_frame || g_footer_shown_frame + 1 == g_frame) shift += g_footer_lag;
        if (shift == 0.0f) return n;
        for (int i = 0; i < 6; i++) q[i].x += shift;
        return footer_band_on(q, right, shift, n);
    }

    if (top < FOOTER_TOP) return n;
    *handled = 1;
    if (g_footer_hints_n < FOOTER_HINTS) g_footer_hints_now[g_footer_hints_n++] = (footer_place){found, left, left, 0};
    const float to = place ? place->to : left;
    const float scale = (right - left) / p->w, scale_down = (bottom - top) / p->h, middle_x = (left + right) / 2.0f;
    const float middle_u = (u_low + u_high) / 2.0f;
    g_footer_hint_scale = scale;
    g_footer_hint_piece = found;
    g_footer_hint_left = left;
    for (int i = 0; i < 6; i++) {
        /* One with more room under it on the sheet grows down on the screen too. */
        const int lower_row = sheet_h - q[i].v > p->v + p->h / 2.0f;
        q[i].x = q[i].x < middle_x ? to : to + p->nw * scale;
        q[i].u = q[i].u < middle_u ? p->nu : p->nu + p->nw;
        q[i].v = sheet_h - (lower_row ? p->nv + p->nh : p->nv);
        if (lower_row) q[i].y += (p->nh - p->h) * scale_down;
    }
    return n;
}

/* With a hint of the game's just drawn as `q`, the extra hints asked for, once a frame: each counts
 * as drawn at a place of its own before the game's, and is shown where that place was learnt to go,
 * as tall as `q` and at its scale. Gives back how many vertices there are now. */
static int footer_extras(const gl_vertex *q, float sheet_h, int n) {
    const unsigned mask = (g_footer_extras_frame + 2 >= g_frame ? g_footer_extras_asked : 0u) | g_footer_extras_with;
    if (!mask || g_footer_extras_done == g_frame + 1) return n;
    g_footer_extras_done = g_frame + 1;
    float left = q[0].x, right = left, top = q[0].y, bottom = top;
    for (int i = 1; i < 6; i++) {
        left = fminf(left, q[i].x);
        right = fmaxf(right, q[i].x);
        top = fminf(top, q[i].y);
        bottom = fmaxf(bottom, q[i].y);
    }
    const float middle_x = (left + right) / 2.0f, middle_y = (top + bottom) / 2.0f;
    const float scale = g_footer_hint_scale;
    for (int i = 0; i < g_footer_piece_count; i++) {
        const footer_piece *p = &g_footer_pieces[i];
        if (!p->extra || p->extra > FOOTER_EXTRAS || !(mask & (1u << (p->extra - 1)))) continue;
        const float at = FOOTER_EXTRA_LEFT - (float)p->extra;
        if (g_footer_hints_n < FOOTER_HINTS) g_footer_hints_now[g_footer_hints_n++] = (footer_place){i, at, at, 0};
        /* Its place is kept as a distance from the hint drawn with it, so that it goes where that
         * goes, sliding in and out with the game's. */
        const int e = p->extra - 1;
        const footer_place *place = footer_place_of(i, at);
        const footer_place *beside = footer_place_of(g_footer_hint_piece, g_footer_hint_left);
        if (place && beside) {
            g_footer_extra_from[e] = g_footer_hint_piece + 1;
            g_footer_extra_dx[e] = place->to - beside->to;
        }
        if (g_footer_extra_from[e] != g_footer_hint_piece + 1 || n + 6 > (int)(sizeof g_vb / sizeof g_vb[0])) continue;
        const float x = left + g_footer_extra_dx[e];
        for (int k = 0; k < 6; k++) {
            gl_vertex v = q[k];
            const int on_right = q[k].x > middle_x, lower = q[k].y > middle_y;
            v.x = on_right ? x + p->nw * scale : x;
            v.u = on_right ? p->nu + p->nw : p->nu;
            v.y = lower ? top + p->nh * scale : top;
            v.v = sheet_h - (lower ? p->nv + p->nh : p->nv);
            g_vb[n + k] = v;
        }
        n += 6;
    }
    return n;
}

/* Every quad of a draw from the footer's sheet. `handled` is set if any was the footer's, which then
 * needs nothing more done to it. */
static int footer_draw(int n, float sheet_h, int *handled) {
    const int drawn = n;
    int first_hint = -1;
    for (int i = 0; i + 6 <= drawn; i += 6) {
        const int hints_before = g_footer_hints_n;
        n = footer_piece_drawn(g_vb + i, sheet_h, n, handled);
        if (first_hint < 0 && g_footer_hints_n > hints_before) first_hint = i;
    }
    if (first_hint >= 0) n = footer_extras(g_vb + first_hint, sheet_h, n);
    return n;
}

/* At the end of a frame: learns where this frame's pieces go, once its hints have stayed the same. */
static void plan_footer(void) {
    footer_place *h = g_footer_hints_now;
    const int n = g_footer_band_now ? g_footer_hints_n : 0;
    /* A window of another width has other places. */
    static int learnt_for_w;
    if (g_show_w != learnt_for_w) {
        learnt_for_w = g_show_w;
        footer_places_load();
    }
    for (int i = 1; i < n; i++) {
        for (int j = i; j > 0 && h[j].left < h[j - 1].left; j--) {
            const footer_place kept = h[j];
            h[j] = h[j - 1];
            h[j - 1] = kept;
        }
    }
    static footer_place before[FOOTER_HINTS];
    static int before_n = -1, steady;
    float band_left = 0.0f;
    int have_band = 0, band_piece = -1;
    for (int i = 0; i < g_footer_bands_n; i++) {
        if (g_footer_pieces[g_footer_bands_now[i].piece].w >= 500.0f) {
            band_left = g_footer_bands_now[i].left;
            band_piece = g_footer_bands_now[i].piece;
            have_band = 1;
        }
    }
    /* Steady when the hints and the band are where they were: as a screen opens, the game slides the
     * band in after its hints have stopped. */
    static float before_band = -1e9f;
    int same = n == before_n && fabsf(band_left - before_band) < FOOTER_SAME;
    for (int i = 0; same && i < n; i++) same = before[i].piece == h[i].piece && fabsf(before[i].left - h[i].left) < FOOTER_SAME;
    steady = same ? steady + 1 : 1;
    memcpy(before, h, sizeof(footer_place) * (size_t)n);
    before_n = n;
    before_band = band_left;

    /* The extras that come with a hint of the game's, if it was drawn. */
    g_footer_extras_with = 0;
    for (int i = 0; i < g_footer_hints_n; i++) {
        const footer_piece *p = &g_footer_pieces[g_footer_hints_now[i].piece];
        for (int e = 0; e < FOOTER_EXTRAS && !p->extra; e++) {
            if (g_footer_extra_with[e] && fabsf(p->u - g_footer_extra_with_u[e]) < FOOTER_SAME && fabsf(p->v - g_footer_extra_with_v[e]) < FOOTER_SAME)
                g_footer_extras_with |= 1u << e;
        }
    }
    /* The next frame finds its places among those of these hints. */
    g_footer_set = footer_set_of(h, n);
    /* Learnt once steady, and again whenever a place is missing, as after the window changed. Each set
     * of hints has its own, so a frame of the change between two sets, learnt, harms neither. */
    int missing = have_band && footer_place_in(band_piece, band_left, g_footer_set) == NULL;
    for (int i = 0; i < n; i++) missing |= footer_place_in(h[i].piece, h[i].left, g_footer_set) == NULL;

    if (n > 0 && have_band && steady >= FOOTER_STEADY && (steady == FOOTER_STEADY || missing)) {
        /* The last hint ends where its place on the sheet did, or where its words do if longer, but
         * not past the window's edge. */
        const footer_piece *last = &g_footer_pieces[h[n - 1].piece];
        /* In a wider window they go with its right edge, as the rest of what is at the right does. */
        const float extra = g_wide && g_show_w > PSP_W ? (float)(g_show_w - PSP_W) / 2.0f : 0.0f;
        const float window_edge = (float)PSP_W + extra;
        float cursor = fminf(h[n - 1].left + fmaxf(last->w, last->nw) + extra, window_edge - FOOTER_EDGE);
        for (int i = n - 1; i >= 0; i--) {
            h[i].to = cursor - g_footer_pieces[h[i].piece].nw;
            cursor = h[i].to - FOOTER_GAP;
            footer_learn(h[i].piece, h[i].left, h[i].to);
        }
        /* The band and its line art keep their distances from each other. */
        const float shift = h[0].to - FOOTER_BAND_LEAD - band_left;
        for (int i = 0; i < g_footer_bands_n; i++) {
            footer_learn(g_footer_bands_now[i].piece, g_footer_bands_now[i].left, g_footer_bands_now[i].left + shift);
        }
        fprintf(stderr, "gl: footer of %d hints laid out, band moved %.1f\n", n, shift);
        if (g_footer_store_dirty) footer_store_write();
    }
    if (!g_footer_band_now) g_footer_shift = 0.0f;
    g_footer_hints_n = 0;
    g_footer_bands_n = 0;
    g_footer_band_now = 0;
}

/* A screenshot asked for, taken at the next present from what is shown, at full size. */
static psp_screenshot_fn volatile g_screenshot_fn;
static volatile LONG g_screenshot_asked;
void psp_render_gl_take_screenshot(psp_screenshot_fn fn) {
    g_screenshot_fn = fn;
    InterlockedExchange(&g_screenshot_asked, 1);
}

static void take_screenshot(GLuint source_fbo, int x, int y, int w, int h) {
    const psp_screenshot_fn fn = g_screenshot_fn;
    uint8_t *pixels = fn && w > 0 && h > 0 ? malloc((size_t)w * h * 4) : NULL;
    if (!pixels) return;
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, source_fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    /* Rows are kept top first, so they come out in the order a picture file wants. */
    glReadPixels(x, y, w, h, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
    fn(pixels, w, h);
    free(pixels);
}

void psp_render_gl_set_wide(int on) {
    g_wide = on;
    g_canvas_w = on ? WIDE_CANVAS_W : LOGICAL;
    g_canvas_h = on ? WIDE_CANVAS_H : LOGICAL;
    g_canvas_x = on ? (WIDE_CANVAS_W - PSP_W) / 2 : 0;
    g_canvas_y = on ? (WIDE_CANVAS_H - PSP_H) / 2 : 0;
}

void psp_render_gl_set_wide_view(int view) { g_wide_view = view; }

void psp_render_gl_set_scale(int scale) {
    if (scale < 1 || scale > 8) return;
    g_scale_asked = scale;
    /* Once there is something to draw on, it changes between frames. */
    if (!g_scale_taken_up) g_scale = scale;
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
    U.uvat = p_glGetUniformLocation(g_program, "u_uvat");
    U.canvas_at = p_glGetUniformLocation(g_program, "u_canvas_at");
    U.canvas_half = p_glGetUniformLocation(g_program, "u_canvas_half");
    U.zoom = p_glGetUniformLocation(g_program, "u_zoom");
    U.blur = p_glGetUniformLocation(g_program, "u_blur");
    U.blur_lod = p_glGetUniformLocation(g_program, "u_blur_lod");
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

/* A picture drawn at several times the window's size, shrunk in one go, only reads a few of the
 * pixels under each of the window's, and its edges come out jagged. Halved first, as often as it
 * is still twice the width wanted, each window pixel is the average of all of those under it: a
 * linear copy to half the size takes the middle of each two by two. */
#define HALVINGS 3
static GLuint g_half_fbo[HALVINGS], g_half_tex[HALVINGS];
static int g_half_w[HALVINGS], g_half_h[HALVINGS];

static GLuint halved_towards(GLuint source, int *x, int *y, int *w, int *h, int wanted_w) {
    for (int i = 0; i < HALVINGS && wanted_w > 0 && *w >= wanted_w * 2; i++) {
        const int hw = *w / 2, hh = *h / 2;
        if (hw <= 0 || hh <= 0) break;
        if (g_half_w[i] != hw || g_half_h[i] != hh) {
            if (g_half_fbo[i]) {
                p_glDeleteFramebuffers(1, &g_half_fbo[i]);
                glDeleteTextures(1, &g_half_tex[i]);
            }
            g_half_fbo[i] = make_color_target(&g_half_tex[i], hw, hh, GL_LINEAR);
            g_half_w[i] = hw;
            g_half_h[i] = hh;
        }
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, source);
        p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_half_fbo[i]);
        p_glBlitFramebuffer(*x, *y, *x + hw * 2, *y + hh * 2, 0, 0, hw, hh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        source = g_half_fbo[i];
        *x = *y = 0;
        *w = hw;
        *h = hh;
    }
    return source;
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

/* Every target goes, to be made again at the new size as the game next draws on it. */
static void take_up_scale(void) {
    const int scale = g_scale_asked;
    if (scale == g_scale) return;
    for (int i = 0; i < MAX_TARGETS; i++) {
        gl_target *t = &g_targets[i];
        if (!t->used) continue;
        p_glDeleteFramebuffers(1, &t->fbo);
        glDeleteTextures(1, &t->color);
        p_glDeleteRenderbuffers(1, &t->depth);
        memset(t, 0, sizeof *t);
    }
    g_scale = scale;
    fprintf(stderr, "gl: render scale %d\n", scale);
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
        t->fbo = make_color_target(&t->color, g_canvas_w * g_scale, g_canvas_h * g_scale, GL_LINEAR);
        p_glGenRenderbuffers(1, &t->depth);
        p_glBindRenderbuffer(GL_RENDERBUFFER, t->depth);
        p_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, g_canvas_w * g_scale, g_canvas_h * g_scale);
        p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, t->depth);
        if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            fprintf(stderr, "gl: incomplete framebuffer\n");
    }
    t->used = 1;
    t->shown = 0;
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
    glViewport(0, 0, g_canvas_w * g_scale, g_canvas_h * g_scale);
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

/* What has been read from files, kept: a game reloads the same textures often, and a file
 * is too large to read again each time. A texture with no file is remembered as well. */
#define REPLACE_CACHE 4096

typedef struct {
    uint32_t colours, shape;
    int w, h;
    uint32_t *pixels;
    int glyphs;   /* from a file of shapes only */
} replacement;

/* Whether the file read last was of shapes only. */
static int g_read_glyphs;

static replacement g_replace_cache[REPLACE_CACHE];
static int g_replace_cached;

static FILE *open_replacement(uint32_t hash, const char *kind, uint32_t size[2]) {
    char path[1200];
    FILE *f = NULL;
    if (g_note_dir[0] && !strcmp(kind, "rgba")) {
        snprintf(path, sizeof path, "%s\\%08X.%s", g_note_dir, hash, kind);
        f = fopen(path, "rb");
    }
    if (!f && g_vector_dir[0] && !strcmp(kind, "rgba")) {
        const char *second = strchr(g_vector_dir, '|');
        const int first_len = second ? (int)(second - g_vector_dir) : (int)strlen(g_vector_dir);
        snprintf(path, sizeof path, "%.*s\\%08X.%s", first_len, g_vector_dir, hash, kind);
        f = fopen(path, "rb");
        if (!f && second) {
            snprintf(path, sizeof path, "%s\\%08X.%s", second + 1, hash, kind);
            f = fopen(path, "rb");
        }
    }
    if (!f && g_language_dir[0] && !strcmp(kind, "rgba")) {
        snprintf(path, sizeof path, "%s\\%08X.%s", g_language_dir, hash, kind);
        f = fopen(path, "rb");
    }
    if (!f && g_replace_dir[0]) {
        snprintf(path, sizeof path, "%s\\%08X.%s", g_replace_dir, hash, kind);
        f = fopen(path, "rb");
    }
    if (!f) return NULL;
    if (fread(size, 4, 2, f) == 2 && size[0] && size[0] <= REPLACE_MAX_SIDE && size[1] && size[1] <= REPLACE_MAX_SIDE) {
        fprintf(stderr, "gl: texture replaced by %s\n", path);
        return f;
    }
    fclose(f);
    return NULL;
}

/* The first folder's card_signs.rgba, if it has one: two signs, one above the other, drawn in
 * place of a listed picture's two regions instead of their changing places. */
static uint32_t *read_card_signs(const char *first, int first_len, int *w, int *h) {
    char path[1200];
    snprintf(path, sizeof path, "%.*s\\card_signs.rgba", first_len, first);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint32_t size[2] = {0, 0};
    uint32_t *pixels = NULL;
    if (fread(size, 4, 2, f) == 2 && size[0] && size[0] <= 1024 && size[1] && size[1] <= 2048 && size[1] % 2 == 0) {
        const size_t count = (size_t)size[0] * size[1];
        pixels = (uint32_t *)malloc(count * sizeof *pixels);
        if (pixels && fread(pixels, 4, count, f) != count) {
            free(pixels);
            pixels = NULL;
        }
    }
    fclose(f);
    *w = (int)size[0];
    *h = (int)size[1];
    return pixels;
}

/* Two regions of a picture change places, if the list says so. Files are bottom row first. */
static void swap_regions(uint32_t hash, uint32_t *pixels, int file_w, int file_h) {
    const char *second = strchr(g_vector_dir, '|');
    if (!second) return;
    char path[1200];
    snprintf(path, sizeof path, "%s\\swap_regions.txt", second + 1);
    FILE *f = fopen(path, "r");
    if (!f) return;
    unsigned listed;
    int ax, ay, bx, by, w, h, original_w;
    while (fscanf(f, "%x %d %d %d %d %d %d %d", &listed, &ax, &ay, &bx, &by, &w, &h, &original_w) == 8) {
        if (listed != hash || original_w <= 0) continue;
        const int scale = file_w / original_w;
        int sw, sh;
        uint32_t *signs = read_card_signs(g_vector_dir, (int)(second - g_vector_dir), &sw, &sh);
        if (signs) {
            /* Each region takes its half of the signs, scaled to it: the first the upper half. */
            const int rw = w * scale, rh = h * scale, half = sh / 2;
            for (int k = 0; k < 2; k++) {
                const int rx = (k ? bx : ax) * scale, ry = (k ? by : ay) * scale;
                for (int row = 0; row < rh; row++) {
                    const int dest_row = file_h - 1 - (ry + row);
                    const int from_top = k * half + row * half / rh;
                    if (dest_row < 0 || dest_row >= file_h) continue;
                    const uint32_t *from = signs + (size_t)(sh - 1 - from_top) * sw;
                    for (int col = 0; col < rw && rx + col < file_w; col++) pixels[(size_t)dest_row * file_w + rx + col] = from[col * sw / rw];
                }
            }
            free(signs);
            break;
        }
        for (int row = 0; row < h * scale; row++) {
            uint32_t *a = pixels + (file_h - 1 - (ay * scale + row)) * file_w + ax * scale;
            uint32_t *b = pixels + (file_h - 1 - (by * scale + row)) * file_w + bx * scale;
            for (int col = 0; col < w * scale; col++) {
                const uint32_t kept = a[col];
                a[col] = b[col];
                b[col] = kept;
            }
        }
        break;
    }
    fclose(f);
}

static uint32_t *read_replacement(uint32_t colours, uint32_t shape, int *w, int *h) {
    uint32_t size[2] = {0, 0};
    FILE *f = open_replacement(colours, "rgba", size);
    const int whole = f != NULL;
    if (!f) f = open_replacement(shape, "alpha", size);
    if (!f) return NULL;
    g_read_glyphs = !whole;

    const size_t count = (size_t)size[0] * size[1];
    uint32_t *pixels = (uint32_t *)malloc(count * sizeof *pixels);
    int ok = 0;
    if (pixels) {
        if (whole) {
            ok = fread(pixels, 4, count, f) == count;
        } else {
            uint8_t *alpha = (uint8_t *)malloc(count);
            if (alpha && fread(alpha, 1, count, f) == count) {
                for (size_t i = 0; i < count; i++) pixels[i] = ((uint32_t)alpha[i] << 24) | 0x00FFFFFFu;
                ok = 1;
            }
            free(alpha);
        }
    }
    fclose(f);
    if (!ok) {
        free(pixels);
        return NULL;
    }
    *w = (int)size[0];
    *h = (int)size[1];
    if (whole) swap_regions(colours, pixels, *w, *h);
    return pixels;
}

/* A PNG with no compression, which needs nothing but a checksum routine. */
static uint32_t png_crc(uint32_t crc, const uint8_t *p, size_t n) {
    static uint32_t table[256];
    if (!table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    }
    for (size_t i = 0; i < n; i++) crc = table[(crc ^ p[i]) & 255] ^ (crc >> 8);
    return crc;
}

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void png_chunk(FILE *f, const char *kind, const uint8_t *data, uint32_t n) {
    uint8_t head[8];
    put_be32(head, n);
    memcpy(head + 4, kind, 4);
    fwrite(head, 1, 8, f);
    if (n) fwrite(data, 1, n, f);
    uint32_t crc = png_crc(0xFFFFFFFFu, head + 4, 4);
    if (n) crc = png_crc(crc, data, n);
    uint8_t tail[4];
    put_be32(tail, crc ^ 0xFFFFFFFFu);
    fwrite(tail, 1, 4, f);
}

#define STORED_BLOCK 65535u

/* Each texture with no larger version, once, named for the hash a replacement would use.
 * In the order the texture has in memory, which for this kind of game can be upside down. */
static void dump_texture(uint32_t colours, int w, int h) {
    char path[1200];
    snprintf(path, sizeof path, "%s\\%08X.png", g_dump_dir, colours);
    FILE *f = fopen(path, "rb");
    if (f) {
        fclose(f);
        return;
    }
    const size_t row = (size_t)w * 4 + 1, raw_size = row * (size_t)h;
    const size_t blocks = raw_size / STORED_BLOCK + 1;
    uint8_t *z = (uint8_t *)malloc(2 + raw_size + blocks * 5 + 4);
    uint8_t *raw = (uint8_t *)malloc(raw_size);
    f = fopen(path, "wb");
    if (z && raw && f) {
        uint32_t a1 = 1, a2 = 0;
        for (int y = 0; y < h; y++) {
            uint8_t *line = raw + row * (size_t)y;
            line[0] = 0;
            memcpy(line + 1, g_decode + y * w, (size_t)w * 4);
        }
        size_t at = 0, out = 0;
        z[out++] = 0x78;
        z[out++] = 0x01;
        while (at < raw_size) {
            const uint32_t n = raw_size - at > STORED_BLOCK ? STORED_BLOCK : (uint32_t)(raw_size - at);
            z[out++] = at + n == raw_size;
            z[out++] = (uint8_t)n;
            z[out++] = (uint8_t)(n >> 8);
            z[out++] = (uint8_t)~n;
            z[out++] = (uint8_t)(~n >> 8);
            memcpy(z + out, raw + at, n);
            for (uint32_t i = 0; i < n; i++) {
                a1 = (a1 + raw[at + i]) % 65521u;
                a2 = (a2 + a1) % 65521u;
            }
            out += n;
            at += n;
        }
        put_be32(z + out, (a2 << 16) | a1);
        out += 4;

        static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        fwrite(signature, 1, 8, f);
        uint8_t ihdr[13];
        put_be32(ihdr, (uint32_t)w);
        put_be32(ihdr + 4, (uint32_t)h);
        ihdr[8] = 8;
        ihdr[9] = 6;
        ihdr[10] = ihdr[11] = ihdr[12] = 0;
        png_chunk(f, "IHDR", ihdr, 13);
        png_chunk(f, "IDAT", z, (uint32_t)out);
        png_chunk(f, "IEND", NULL, 0);
    }
    if (f) fclose(f);
    free(z);
    free(raw);
}

/* Clear texels keep a colour of their own, often white, and linear filtering blends it into the
 * edge of whatever they border: a dark line on a clear ground shows a light rim. Each clear texel
 * next to a coloured one takes the average colour of its coloured neighbours, still clear, and the
 * pass is repeated so the colour reaches a little further. Black clear texels are left: they make
 * no rim, and a piece the game adds to the screen, alpha or not, must add nothing there. */
#define BLEED_PASSES 2
static void bleed_into_clear(uint32_t *pixels, int w, int h) {
    unsigned char *coloured = malloc((size_t)w * h);
    if (!coloured) return;
    /* 1: coloured, a source. 0: clear with a colour, to be given its neighbours'. 255: black and clear, kept. */
    for (int i = 0; i < w * h; i++) coloured[i] = (pixels[i] >> 24) != 0 ? 1 : (pixels[i] & 0xFFFFFFu) ? 0 : 255;
    for (int pass = 1; pass <= BLEED_PASSES; pass++) {
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                const int at = y * w + x;
                if (coloured[at]) continue;   /* a source, done, or black and kept */
                unsigned r = 0, g = 0, b = 0, n = 0;
                const int around[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
                for (int k = 0; k < 4; k++) {
                    const int ax = around[k][0], ay = around[k][1];
                    if (ax < 0 || ay < 0 || ax >= w || ay >= h) continue;
                    const int from = ay * w + ax;
                    if (!coloured[from] || coloured[from] == 255 || coloured[from] == pass + 1) continue;
                    const uint32_t p = pixels[from];
                    r += p & 0xFF; g += (p >> 8) & 0xFF; b += (p >> 16) & 0xFF; n++;
                }
                if (!n) continue;
                pixels[at] = (r / n) | (g / n) << 8 | (b / n) << 16;
                coloured[at] = (unsigned char)(pass + 1);  /* a source only from the next pass on */
            }
        }
    }
    free(coloured);
}

/* The file standing in for what is in g_decode, or NULL. */
static int g_replacement_glyphs;   /* whether replacement_for's last answer was of shapes only */

static const uint32_t *replacement_for(int w, int h, int *out_w, int *out_h, uint32_t *out_colours) {
    g_replacement_glyphs = 0;
    if ((!g_replace_dir[0] && !g_language_dir[0] && !g_dump_dir[0]) || w < REPLACE_MIN_SIDE || h < REPLACE_MIN_SIDE) return NULL;

    /* A picture is known by its colours, and letters by their shape alone. */
    uint32_t colours = 2166136261u, shape = 2166136261u;
    for (int i = 0; i < w * h; i++) {
        const uint32_t p = g_decode[i], alpha = p >> 24;
        const uint32_t solid = alpha > REPLACE_SOLID_ALPHA ? 1u : 0u;
        const uint32_t key = alpha ? solid | ((p >> 5) & 7u) << 1 | ((p >> 13) & 7u) << 4 | ((p >> 21) & 7u) << 7 | 1u << 10 : 0u;
        colours = (colours ^ key) * 16777619u;
        shape = (shape ^ solid) * 16777619u;
    }
    *out_colours = colours;
    /* Saved as the game has it, whether or not a file stands in for it on the screen. */
    if (g_dump_dir[0]) dump_texture(colours, w, h);
    if (!g_replace_dir[0] && !g_language_dir[0]) return NULL;

    for (int i = 0; i < g_replace_cached; i++) {
        const replacement *r = &g_replace_cache[i];
        if (r->colours == colours && r->shape == shape) {
            if (!r->pixels) return NULL;  /* and it has been saved already, if that is wanted */
            *out_w = r->w;
            *out_h = r->h;
            g_replacement_glyphs = r->glyphs;
            return r->pixels;
        }
    }
    int file_w = 0, file_h = 0;
    g_read_glyphs = 0;
    uint32_t *pixels = read_replacement(colours, shape, &file_w, &file_h);
    const int glyphs = pixels && g_read_glyphs;
    if (pixels) bleed_into_clear(pixels, file_w, file_h);
    if (g_replace_cached < REPLACE_CACHE) {
        replacement *r = &g_replace_cache[g_replace_cached++];
        r->colours = colours;
        r->shape = shape;
        r->w = file_w;
        r->h = file_h;
        r->pixels = pixels;
        r->glyphs = glyphs;
    }
    if (!pixels) return NULL;
    *out_w = file_w;
    *out_h = file_h;
    g_replacement_glyphs = glyphs;
    return pixels;
}

static void decode_texture(gl_texture *e, const psp_draw_state *st) {
    const int w = st->tex_w, h = st->tex_h;
    if (w > TEX_MAX_SIDE || h > TEX_MAX_SIDE) return;
    const uint64_t began = gl_now_us();
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            g_decode[y * w + x] = psp_tex_fetch(st, x, y);
    int up_w = w, up_h = h;
    e->colours = 0;
    const uint32_t *pixels = replacement_for(w, h, &up_w, &up_h, &e->colours);
    e->replaced = pixels != NULL;
    e->glyphs = pixels != NULL && g_replacement_glyphs;
    if (!pixels) pixels = g_decode;
    glBindTexture(GL_TEXTURE_2D, e->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, up_w, up_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    e->up_w = up_w;
    e->up_h = up_h;
    e->mipped = 0;
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

/* Another language: what was read from files and made from them is for the last one. */
static void take_up_dir(void *volatile *where, char *dir, size_t size) {
    if (!*where) return;
    char *asked = (char *)InterlockedExchangePointer(where, NULL);
    if (!asked) return;
    if (strcmp(asked, dir) != 0) {
        snprintf(dir, size, "%s", asked);
        for (int i = 0; i < g_replace_cached; i++) free(g_replace_cache[i].pixels);
        g_replace_cached = 0;
        forget_textures();
    }
    free(asked);
}

static GLuint g_note_targets;      /* targets.rgba of the note style, once read */
static int g_note_targets_read;
static float g_note_targets_side;   /* its side, in the units of its texture coordinates */

static void take_up_language(void) {
    take_up_dir(&g_language_asked, g_language_dir, sizeof g_language_dir);
    take_up_dir(&g_vector_asked, g_vector_dir, sizeof g_vector_dir);
    if (g_note_asked) {
        char before[sizeof g_note_dir];
        snprintf(before, sizeof before, "%s", g_note_dir);
        take_up_dir(&g_note_asked, g_note_dir, sizeof g_note_dir);
        if (strcmp(before, g_note_dir) != 0) {
            if (g_note_targets) glDeleteTextures(1, &g_note_targets);
            g_note_targets = 0;
            g_note_targets_read = 0;
        }
    }
    read_footer_pieces();
}

static gl_texture *get_texture(const psp_draw_state *st) {
    take_up_language();
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
                         const psp_draw_state *st, int smooth) {
    glBindTexture(GL_TEXTURE_2D, tex);
    const int min_linear = st->tex_min_linear || smooth, mag_linear = st->tex_mag_linear || smooth;
    /* A replacement drawn larger than the game's is shown smaller than it is, often by four times:
     * read from its smaller levels, or its edges come out jagged. */
    const int min_mode = smooth > 1 ? 2 : min_linear;
    if (*min_seen != min_mode) {
        *min_seen = min_mode;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min_mode == 2 ? GL_LINEAR_MIPMAP_LINEAR : min_linear ? GL_LINEAR : GL_NEAREST);
    }
    if (*mag_seen != mag_linear) {
        *mag_seen = mag_linear;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag_linear ? GL_LINEAR : GL_NEAREST);
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
    g_tex_colours = 0;
    g_tex_bound = NULL;
    if (!st->tex_enable || st->clear || st->tex_w <= 0 || st->tex_h <= 0) return 0;
    if (in_vram(st->tex_addr)) {
        gl_target *t = find_target(st->tex_addr);
        if (t && t != g_bound) {
            int unknown_min = -1, unknown_mag = -1, unknown_u = -1, unknown_v = -1;
            bind_sampler(t->color, &unknown_min, &unknown_mag, &unknown_u, &unknown_v, st, 0);
            p_glUniform2f(U.uvscale, 1.0f / (float)g_canvas_w, 1.0f / (float)g_canvas_h);
            p_glUniform2f(U.uvat, (float)g_canvas_x / (float)g_canvas_w, (float)g_canvas_y / (float)g_canvas_h);
            return 1;
        }
    }
    gl_texture *e = get_texture(st);
    if (!e) return 0;
    g_tex_colours = e->colours;
    g_tex_bound = e;
    int smooth = e->replaced;
    /* Not a font's letters, which sit close on their sheet: smaller levels would mix in their neighbours'. */
    if (e->replaced && !e->glyphs && e->up_w > st->tex_w) {
        if (!e->mipped) {
            glBindTexture(GL_TEXTURE_2D, e->tex);
            p_glGenerateMipmap(GL_TEXTURE_2D);
            e->mipped = 1;
        }
        smooth = 2;
    }
    bind_sampler(e->tex, &e->min_linear, &e->mag_linear, &e->clamp_u, &e->clamp_v, st, smooth);
    /* How a replaced texture is used, once: its look depends on this as much as on the file. */
    if (e->replaced && !e->use_logged) {
        e->use_logged = 1;
        fprintf(stderr, "gl: replaced %dx%d at 0x%08X: func %d alpha %d blend %d src %d dst %d op %d filter %d %d\n", st->tex_w, st->tex_h,
                st->tex_addr, st->tex_func, st->tex_alpha, st->blend, st->blend_src, st->blend_dst, st->blend_op, st->tex_min_linear,
                st->tex_mag_linear);
    }
    p_glUniform2f(U.uvscale, 1.0f / (float)st->tex_w, 1.0f / (float)st->tex_h);
    p_glUniform2f(U.uvat, 0.0f, 0.0f);
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

/* A fade is a few vertices with no texture over the whole screen, whatever its w: some are drawn
 * in the 3D scene, close to the eye. It goes over the whole canvas. */
static int stretch_fade(int n) {
    float left = 1e9f, right = -1e9f, top = 1e9f, bottom = -1e9f;
    for (int i = 0; i < n; i++) {
        if (g_vb[i].w == 0.0f) return 0;
        const float x = g_vb[i].x / g_vb[i].w, y = g_vb[i].y / g_vb[i].w;
        if (x < left) left = x;
        if (x > right) right = x;
        if (y < top) top = y;
        if (y > bottom) bottom = y;
    }
    if (left > COVERS_NEAR || top > COVERS_NEAR || right < PSP_W - COVERS_NEAR || bottom < PSP_H - COVERS_NEAR) return 0;
    for (int i = 0; i < n; i++) {
        const float w = g_vb[i].w;
        g_vb[i].x = (g_vb[i].x / w < (float)PSP_W / 2.0f ? (float)-g_canvas_x : (float)(g_canvas_w - g_canvas_x)) * w;
        g_vb[i].y = (g_vb[i].y / w < (float)PSP_H / 2.0f ? (float)-g_canvas_y : (float)(g_canvas_h - g_canvas_y)) * w;
    }
    return 1;
}

/* Beside a band that ends at the screen's edge, its last column repeated up to the window's edge.
 * Gives back how many vertices there are now. */
static int continue_band(int n, float from, float to, float u_at_edge) {
    return continue_quad(g_vb, n, from, to, u_at_edge);
}

static int clamp_int(int v, int low, int high) { return v < low ? low : v > high ? high : v; }

/* The scissor each draw uses, in target pixels. On the game's own picture it never goes past what
 * the window shows of the canvas: in a wide window half of its rows are never seen, and drawing
 * them would cost as much as the rest. */
#define SHOWN_MARGIN 2
static int g_sc_x, g_sc_y, g_sc_w, g_sc_h;

static void set_scissor(int x, int y, int w, int h) {
    if (g_wide && g_bound && g_bound->shown) {
        const int vx = ((g_canvas_w - g_show_w) / 2 - SHOWN_MARGIN) * g_scale, vy = ((g_canvas_h - g_show_h) / 2 - SHOWN_MARGIN) * g_scale;
        const int vw = (g_show_w + 2 * SHOWN_MARGIN) * g_scale, vh = (g_show_h + 2 * SHOWN_MARGIN) * g_scale;
        const int x1 = x + w < vx + vw ? x + w : vx + vw, y1 = y + h < vy + vh ? y + h : vy + vh;
        x = x > vx ? x : vx;
        y = y > vy ? y : vy;
        w = x1 > x ? x1 - x : 0;
        h = y1 > y ? y1 - y : 0;
    }
    g_sc_x = x;
    g_sc_y = y;
    g_sc_w = w;
    g_sc_h = h;
    glScissor(x, y, w, h);
}

/* Draws the six vertices in the buffer only outside a rectangle of the screen. The blur looks at many
 * points for each pixel, so at a high resolution it must not be spent under the picture that covers
 * it. It keeps within the scissor in use, and leaves it as it was. */
static void draw_around(float left, float top, float right, float bottom) {
    const int w = g_canvas_w * g_scale, h = g_canvas_h * g_scale;
    const int x0 = clamp_int((int)ceilf((left + (float)g_canvas_x) * (float)g_scale), 0, w);
    const int x1 = clamp_int((int)floorf((right + (float)g_canvas_x) * (float)g_scale), x0, w);
    const int y0 = clamp_int((int)ceilf((top + (float)g_canvas_y) * (float)g_scale), 0, h);
    const int y1 = clamp_int((int)floorf((bottom + (float)g_canvas_y) * (float)g_scale), y0, h);
    const int part[4][4] = {
        {0, 0, w, y0}, {0, y1, w, h - y1}, {0, y0, x0, y1 - y0}, {x1, y0, w - x1, y1 - y0},
    };
    const int sx0 = g_sc_x, sy0 = g_sc_y, sx1 = g_sc_x + g_sc_w, sy1 = g_sc_y + g_sc_h;
    for (int i = 0; i < 4; i++) {
        const int px0 = part[i][0] > sx0 ? part[i][0] : sx0, py0 = part[i][1] > sy0 ? part[i][1] : sy0;
        const int px1 = part[i][0] + part[i][2] < sx1 ? part[i][0] + part[i][2] : sx1;
        const int py1 = part[i][1] + part[i][3] < sy1 ? part[i][1] + part[i][3] : sy1;
        if (px1 <= px0 || py1 <= py0) continue;
        glScissor(px0, py0, px1 - px0, py1 - py0);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }
    glScissor(g_sc_x, g_sc_y, g_sc_w, g_sc_h);
}

/* ---- blurred copies --------------------------------------------------------
 *
 * What is past a picture that does not fill the window is a blurred copy of it. The blur is made
 * from one of the texture's smaller levels, which the GPU makes once, and five looks at it: as
 * blurred as 49 looks at the whole picture, at a small part of the cost. A target the game drew
 * into has no smaller levels, and gets the 49 looks. */
#define BLUR_RADIUS 0.045f
#define BLUR_TEXELS 22.0f   /* how many texels across the picture its blur leaves */

static int g_blur_on = 1;
void psp_render_gl_set_blur(int on) { g_blur_on = on; }

static void begin_blur(void) {
    if (!g_blur_on) return;
    gl_texture *e = g_tex_bound;
    float lod = 0.0f;
    if (e && e->up_w > 0) {
        if (!e->mipped) {
            p_glGenerateMipmap(GL_TEXTURE_2D);
            e->mipped = 1;
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        e->min_linear = -1;   /* set again on its next use */
        lod = log2f((float)(e->up_w > e->up_h ? e->up_w : e->up_h) / BLUR_TEXELS);
        if (lod < 0.0f) lod = 0.0f;
    }
    p_glUniform1f(U.blur, BLUR_RADIUS);
    p_glUniform1f(U.blur_lod, lod);
}

static void end_blur(void) {
    p_glUniform1f(U.blur, 0.0f);
    p_glUniform1f(U.blur_lod, 0.0f);
}

/* A menu's background is a 3D plane built only a little past the PSP's screen. In a wider window the
 * vertices at its two ends go on to the window's edges, their texture coordinates going on at the same
 * rate as across the rest of it, so that its pattern carries on rather than stretching. A draw is
 * taken for such a plane if it reaches past both sides of the screen and covers at least half its
 * height. A song's stage has such planes too, as its sky: past it the window would show the flat
 * picture the stage hides on the PSP. What already reaches past the window is left as it is. */
#define PLANE_EDGE_NEAR 2.0f
#define PLANE_MIN_HEIGHT (PSP_H / 2.0f)
#define PLANE_BLUR_MARGIN 1.05f   /* the blurred copy a little larger, so that its own edges stay out of sight */
static void widen_background_plane(int prim, int n, const psp_draw_state *st) {
    if (!g_wide || g_show_w <= PSP_W || g_in_place_now || g_in_place_last || !st->tex_enable || st->through || n < 4) return;
    float left = 1e9f, right = -1e9f, top = 1e9f, bottom = -1e9f;
    for (int i = 0; i < n; i++) {
        const float w = g_vb[i].w;
        if (w <= 0.0f || fabsf(w - 1.0f) <= FLAT_W_NEAR) return;
        const float x = g_vb[i].x / w, y = g_vb[i].y / w;
        left = fminf(left, x);
        right = fmaxf(right, x);
        top = fminf(top, y);
        bottom = fmaxf(bottom, y);
    }
    if (left > 1.0f || right < (float)PSP_W - 1.0f || bottom - top < PLANE_MIN_HEIGHT) return;
    if (g_song_frames > 0) {
        if (!g_blur_on) return;
        /* In a song such a plane is a picture of the stage, as the landscapes that scroll by in some
         * songs: beside it, a blurred copy of it, widened to the window's edges. Only widened, so that it
         * stays in the rows of its own picture: two that scroll by one after the other meet as they do,
         * and one waiting off the screen costs nothing. */
        if (!((prim == PSP_PRIM_TRIANGLE_STRIP && n == 4) || (prim == PSP_PRIM_TRIANGLES && n == 6))) return;
        static const int STRIP[6] = {0, 1, 2, 2, 1, 3};
        const float grow = fmaxf((float)g_show_w / (right - left), 1.0f) * PLANE_BLUR_MARGIN;
        const float middle_x = (left + right) / 2.0f;
        gl_vertex copy[6];
        for (int i = 0; i < 6; i++) {
            copy[i] = g_vb[n == 4 ? STRIP[i] : i];
            const float w = copy[i].w, x = copy[i].x / w;
            copy[i].x = (middle_x + (x - middle_x) * grow) * w;
        }
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        begin_blur();
        p_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof copy, copy, GL_STREAM_DRAW);
        draw_around(fmaxf(left, (float)-g_canvas_x), fmaxf(top, (float)-g_canvas_y), fminf(right, (float)(g_canvas_w - g_canvas_x)),
                    fminf(bottom, (float)(g_canvas_h - g_canvas_y)));
        end_blur();
        setup_depth(st);
        return;
    }
    /* How fast the texture runs across the screen, from the vertices at the two ends. */
    float u_left = 0.0f, u_right = 0.0f;
    int at_left = 0, at_right = 0;
    for (int i = 0; i < n; i++) {
        const float x = g_vb[i].x / g_vb[i].w;
        if (x <= left + PLANE_EDGE_NEAR) u_left += g_vb[i].u, at_left++;
        if (x >= right - PLANE_EDGE_NEAR) u_right += g_vb[i].u, at_right++;
    }
    if (!at_left || !at_right || right - left < 1.0f) return;
    const float rate = (u_right / (float)at_right - u_left / (float)at_left) / (right - left);
    const float reach = (float)(g_show_w - PSP_W) / 2.0f + PLANE_EDGE_NEAR;
    for (int i = 0; i < n; i++) {
        const float w = g_vb[i].w, x = g_vb[i].x / w;
        float to = x;
        if (x <= left + PLANE_EDGE_NEAR && x > -reach) to = -reach;
        else if (x >= right - PLANE_EDGE_NEAR && x < (float)PSP_W + reach) to = (float)PSP_W + reach;
        if (to == x) continue;
        g_vb[i].x = to * w;
        g_vb[i].u += (to - x) * rate;
    }
}

/* The game's flat pieces have a w of 1, while its models, the notes among them, have other values.
 * Most are two triangles; a few, as the meter's ring, are shapes of their own, which only move.
 * Takes the n vertices just put in g_vb, and gives back how many there are now. */
static int place_flat_piece(int prim, int n, const psp_draw_state *st) {
    float left = g_vb[0].x, right = left, top = g_vb[0].y, bottom = top;
    float u_low = g_vb[0].u, u_high = u_low, v_low = g_vb[0].v, v_high = v_low;
    float u_left = g_vb[0].u, u_right = g_vb[0].u;
    for (int i = 0; i < n; i++) {
        if (fabsf(g_vb[i].w - 1.0f) > FLAT_W_NEAR) return n;
        if (g_vb[i].u < u_low) u_low = g_vb[i].u;
        if (g_vb[i].u > u_high) u_high = g_vb[i].u;
        if (g_vb[i].v < v_low) v_low = g_vb[i].v;
        if (g_vb[i].v > v_high) v_high = g_vb[i].v;
        if (g_vb[i].x < left) { left = g_vb[i].x; u_left = g_vb[i].u; }
        if (g_vb[i].x > right) { right = g_vb[i].x; u_right = g_vb[i].u; }
        if (g_vb[i].y < top) top = g_vb[i].y;
        if (g_vb[i].y > bottom) bottom = g_vb[i].y;
    }
    const int frame_kind = frame_piece(st, u_low, u_high, v_low, v_high);
    if (frame_kind != NOT_FRAME) widen_frame_piece(frame_kind, n, left, right, top, bottom);
    const int quad = prim == PSP_PRIM_TRIANGLES && n == 6;
    const int covers = quad && left <= COVERS_NEAR && top <= COVERS_NEAR && right >= PSP_W - COVERS_NEAR && bottom >= PSP_H - COVERS_NEAR;
    if (covers && (!st->tex_enable || u_high - u_low < PICTURE_SPAN || v_high - v_low < PICTURE_SPAN)) {
        /* A veil over the whole screen is over the whole canvas. What comes after it is a dialog. */
        for (int i = 0; i < n; i++) {
            g_vb[i].x = g_vb[i].x < (float)PSP_W / 2.0f ? (float)-g_canvas_x : (float)(g_canvas_w - g_canvas_x);
            g_vb[i].y = g_vb[i].y < (float)PSP_H / 2.0f ? (float)-g_canvas_y : (float)(g_canvas_h - g_canvas_y);
        }
        /* In a song a light veil is a flash, as at the end of a chance time, and the HUD under it stays
         * where it goes; a dark one is the pause menu's. */
        const uint32_t c = g_vb[0].rgba;
        const int dark = (c & 0xFFu) < 0x40u && (c >> 8 & 0xFFu) < 0x40u && (c >> 16 & 0xFFu) < 0x40u;
        if (dark || g_song_frames <= 0) g_veil_now = 1;
        return n;
    }
    if (!quad && left <= COVERS_NEAR && top <= COVERS_NEAR && right >= PSP_W - COVERS_NEAR && bottom >= PSP_H - COVERS_NEAR) {
        /* A flat shape of many vertices over the whole screen is an effect laid over all of it, as the
         * white light over some songs' stages: it is stretched from the middle to the window's edges. */
        const float reach_x = (float)(g_show_w - PSP_W) / 2.0f + COVERS_NEAR, reach_y = (float)(g_show_h - PSP_H) / 2.0f + COVERS_NEAR;
        const float half_w = (float)PSP_W / 2.0f, half_h = (float)PSP_H / 2.0f;
        const float grow_x = reach_x > 0.0f ? fmaxf(1.0f, (half_w + reach_x) / fmaxf(half_w - left, right - half_w)) : 1.0f;
        const float grow_y = reach_y > 0.0f ? fmaxf(1.0f, (half_h + reach_y) / fmaxf(half_h - top, bottom - half_h)) : 1.0f;
        for (int i = 0; i < n; i++) {
            g_vb[i].x = half_w + (g_vb[i].x - half_w) * grow_x;
            g_vb[i].y = half_h + (g_vb[i].y - half_h) * grow_y;
        }
        return n;
    }
    if (covers) {
        /* A picture has nothing past it, so a blurred copy fills the rest of the window, and hides
         * whatever the game left there. In a song it is only the background, which can come and go
         * from frame to frame, so the HUD does not change its place for it. */
        const int in_song = g_song_frames > 0;
        if (!in_song) {
            g_flat_now = 1;
            g_flat_sheet = st->tex_addr;
        }
        if (g_show_w <= PSP_W && g_show_h <= PSP_H) return n;
        /* One larger than the screen may need no copy, if it fills the window by itself. */
        const float past_side = (float)(g_show_w - PSP_W) / 2.0f, past_down = (float)(g_show_h - PSP_H) / 2.0f;
        if (in_song && left <= COVERS_NEAR - past_side && right >= PSP_W + past_side - COVERS_NEAR &&
            top <= COVERS_NEAR - past_down && bottom >= PSP_H + past_down - COVERS_NEAR) return n;
        if (g_card_last && g_show_h > PSP_H) {
            const float tall = (float)g_show_h / (float)PSP_H;
            for (int i = 0; i < n; i++) g_vb[i].y = (float)PSP_H / 2.0f + (g_vb[i].y - (float)PSP_H / 2.0f) * tall;
            return n;
        }
        const float across = (float)g_show_w / (float)PSP_W, down = (float)g_show_h / (float)PSP_H;
        const float grow = across > down ? across : down;
        gl_vertex copy[6];
        for (int i = 0; i < 6; i++) {
            copy[i] = g_vb[i];
            copy[i].x = (float)PSP_W / 2.0f + (g_vb[i].x - (float)PSP_W / 2.0f) * grow;
            copy[i].y = (float)PSP_H / 2.0f + (g_vb[i].y - (float)PSP_H / 2.0f) * grow;
        }
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        begin_blur();
        p_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof copy, copy, GL_STREAM_DRAW);
        /* The hole goes no further than the screen: a picture's quad can be wider than it, with
         * its sides clear, and those must show the blur, not nothing. */
        draw_around(fmaxf(left, 0.0f), fmaxf(top, 0.0f), fminf(right, (float)PSP_W), fminf(bottom, (float)PSP_H));
        end_blur();
        setup_depth(st);
        return n;
    }
    for (int i = 0; i < g_in_place_n; i++) {
        if (g_tex_colours && g_tex_colours == g_in_place[i]) {
            g_in_place_now = 1;
            return n;
        }
    }
    if (g_flat_now && st->tex_addr == g_flat_sheet) g_card_now = 1;
    /* The combo and the points beside it follow the notes. They are told from the sheet that holds
     * them, 512x256, and the rows they are on. */
    /* Rows of the sheet as its picture has them, from the top: the game counts them from the bottom. */
    const float row_top = (float)st->tex_h - v_high, row_bottom = (float)st->tex_h - v_low;
    if (st->tex_w == 512 && st->tex_h == 256 &&
        ((row_top >= -ROWS_NEAR && row_bottom <= 17.0f + ROWS_NEAR) || (row_top >= 57.0f - ROWS_NEAR && row_bottom <= 71.0f + ROWS_NEAR) ||
         (row_top >= 77.0f - ROWS_NEAR && row_bottom <= 92.0f + ROWS_NEAR)))
        return n;
    /* In a song the combo is also put together in a picture of its own, 256x128, and drawn from its
     * corner, 127x64, beside the note it counts. */
    if (g_song_frames > 0 && st->tex_w == 256 && st->tex_h == 128 && u_low <= 1.0f && fabsf(u_high - u_low - COMBO_W) <= 2.0f &&
        fabsf(v_high - v_low - COMBO_H) <= 2.0f)
        return n;

    const float extra_down = (float)(g_show_h - PSP_H) / 2.0f;
    const float extra_side = (float)(g_show_w - PSP_W) / 2.0f;
    const int flat = g_flat_now || g_flat_last;
    const int band = right - left >= BAND_WIDTH;
    const float across_middle = (left + right) / 2.0f;
    /* In a song a wide piece in the middle is a dialog only over the pause menu's veil: the song's title
     * card at its start is one too, and the HUD stays at the edges under it. */
    if (!band && right - left >= DIALOG_WIDTH && fabsf(across_middle - (float)PSP_W / 2.0f) <= DIALOG_OFF_MIDDLE &&
        (g_song_frames <= 0 || g_veil_now))
        g_dialog_now = 1;
    const int in_dialog = (g_dialog_now || g_dialog_last) && bottom > PIN_TOP && top < PIN_BOTTOM;
    /* The score's digits, on their own rows of the sheet the combo is on. */
    const int score_digit = st->tex_w == 512 && st->tex_h == 256 && row_top >= 20.0f && row_bottom <= 40.0f;
    if (quad && band && top >= METER_TOP && top < PIN_BOTTOM && st->tex_h != 512 && g_song_frames <= 0) g_results_now = 1;
    float down = 0.0f, side = 0.0f;
    if (g_show_h > PSP_H) {
        const float middle = (top + bottom) / 2.0f;
        /* What is parked off the screen leaves in any case. */
        if (bottom <= 0.0f) down = -extra_down;
        else if (top >= (float)PSP_H) down = extra_down;
        else if (flat) {
            /* Each piece keeps its size, and goes where the stretched ground has its place. */
            if (g_card_last) down = (middle - (float)PSP_H / 2.0f) * ((float)g_show_h / (float)PSP_H - 1.0f);
        } else if (band) {
            /* A song's and the results' reach higher than the others. */
            if (middle < BAND_TOP) down = -extra_down;
            else if (middle > BAND_BOTTOM && top >= METER_TOP) down = extra_down;
        } else if (bottom <= PIN_TOP) {
            down = -extra_down;
        } else if (top >= PIN_BOTTOM || (right <= METER_RIGHT && top >= METER_TOP && bottom >= METER_BOTTOM)) {
            down = extra_down;
        } else if ((g_results_now || g_results_last) && !in_dialog && top >= RESULT_WORDS_TOP && across_middle >= (float)PSP_W / 2.0f) {
            /* The results' band has that screen's buttons drawn on it, and these are what goes on
             * them, with the words above them. The score, on the left, is not on the band. */
            down = extra_down;
        }
    }
    if (g_show_w > PSP_W && !flat && !g_veil_now && !in_dialog) {
        const int to_right_edge = right >= PSP_W - 1.0f && right <= PSP_W + 1.0f;
        const int from_left_edge = left >= -1.0f && left <= 1.0f;
        if (quad && band && top >= METER_TOP && top < PIN_BOTTOM && st->tex_h == 512) {
            /* A song's band has the meter's ring drawn on it, on the left. It goes there with the
             * rest of the meter, and continues to the right. */
            n = continue_band(n, right, right + 2.0f * extra_side, u_right);
            side = -extra_side;
        } else if (quad && band && top >= METER_TOP && top < PIN_BOTTOM) {
            /* The results' band has the buttons drawn on it, on the right. It goes there with what
             * is written on them, and continues to the left. */
            n = continue_band(n, left, left - 2.0f * extra_side, u_left);
            side = extra_side;
        } else if (quad && band && bottom <= PIN_TOP && (g_results_now || g_results_last)) {
            /* The results' top band has that screen's title plate drawn on its left. It goes to the left
             * edge with it, and continues to the right. */
            n = continue_band(n, right, right + 2.0f * extra_side, u_right);
            side = -extra_side;
        } else if (quad && band) {
            /* A band runs on to the window's edges. What overruns the screen already does. */
            if (from_left_edge) n = continue_band(n, left, left - extra_side, u_left);
            if (to_right_edge) n = continue_band(n, right, right + extra_side, u_right);
        } else if (quad && to_right_edge && right - left >= SIDE_BAND_WIDTH && bottom - top <= THIN_BAND_HEIGHT) {
            /* A long thin piece of a band, from one thing to the screen's edge, has art up to its end
             * that a repeated column would not continue. It takes up the room itself. */
            for (int i = 0; i < n; i++) g_vb[i].x += g_vb[i].x < across_middle ? -extra_side : extra_side;
        } else if (band) {
            /* Too many vertices to continue as a quad: what reaches the screen's edges is drawn on to
             * the window's, its last stretch of art made longer. */
            int moved = 0;
            for (int i = 0; i < n; i++) {
                if (from_left_edge && g_vb[i].x <= left + 0.5f) g_vb[i].x -= extra_side, moved = 1;
                else if (to_right_edge && g_vb[i].x >= right - 0.5f) g_vb[i].x += extra_side, moved = 1;
            }
            static int logged;
            if (moved && logged < 8) {
                logged++;
                fprintf(stderr, "gl: band of %d vertices drawn on to the edges, sheet %08X, rows %.0f-%.0f, at %.0f-%.0f\n", n, g_tex_colours, v_low,
                        v_high, top, bottom);
            }
        } else if (top >= PIN_BOTTOM) {
            /* Words at the bottom are lyrics, one draw to a word, and must stay together. The hints
             * belong to the panel at the right, and only the very left stays at the left. */
            if (n == 6) side = across_middle < BOTTOM_LEFT_END ? -extra_side : extra_side;
            /* In a song the lyrics start beside the meter, and go to the left with it. */
            else if (g_song_frames > 0) side = -extra_side;
        } else if (bottom <= PIN_TOP && across_middle >= (float)PSP_W / 2.0f && !score_digit) {
            /* The frames around what is at the top right are drawn in the band, which stays. */
        } else {
            side = across_middle < (float)PSP_W / 2.0f ? -extra_side : extra_side;
        }
    }
    if (down != 0.0f || side != 0.0f) {
        for (int i = 0; i < n; i++) {
            g_vb[i].x += side;
            g_vb[i].y += down;
        }
    }
    return n;
}

/* 2 asks for a frame to be written down, and 1 is that frame. */
static volatile int g_trace_draws;
static int g_traces;

void psp_render_gl_trace_frame(void) { g_trace_draws = 2; }

static void trace_draw(int prim, const psp_vertex *v, int count, const psp_draw_state *st) {
    float left = 1e9f, top = 1e9f, right = -1e9f, bottom = -1e9f;
    float u_low = 1e9f, v_low = 1e9f, u_high = -1e9f, v_high = -1e9f;
    for (int i = 0; i < count; i++) {
        const float w = v[i].pw != 0.0f ? v[i].pw : 1.0f;
        const float x = v[i].px / w, y = v[i].py / w;
        if (x < left) left = x;
        if (x > right) right = x;
        if (y < top) top = y;
        if (y > bottom) bottom = y;
        if (v[i].u < u_low) u_low = v[i].u;
        if (v[i].u > u_high) u_high = v[i].u;
        if (v[i].v < v_low) v_low = v[i].v;
        if (v[i].v > v_high) v_high = v[i].v;
    }
    fprintf(stderr, "gl: draw uv %.0f,%.0f %.0f,%.0f ", u_low, v_low, u_high, v_high);
    fprintf(stderr, "gl: draw prim %d n %d through %d w %.5f box %.0f,%.0f %.0f,%.0f scissor %d,%d %d,%d target %08X tex %d %08X %dx%d blend %d %d %d ztest %d zwrite %d func %d alpha %d shown %d colour %08X\n",
            prim, count, st->through, v[0].pw, left, top, right, bottom, st->sc_x0, st->sc_y0, st->sc_x1, st->sc_y1, g_target_addr,
            st->tex_enable, st->tex_addr, st->tex_w, st->tex_h, st->blend, st->blend_src, st->blend_dst,
            st->ztest, st->zwrite, st->tex_func, st->atest, g_bound && g_bound->shown, v[0].rgba);
}

/* Some songs have a flat picture for their whole background, larger than the screen. */
static int covers_screen(const psp_vertex *v, int count) {
    float left = v[0].px, right = left, top = v[0].py, bottom = top;
    for (int i = 1; i < count; i++) {
        if (v[i].px < left) left = v[i].px;
        if (v[i].px > right) right = v[i].px;
        if (v[i].py < top) top = v[i].py;
        if (v[i].py > bottom) bottom = v[i].py;
    }
    return left <= COVERS_NEAR && top <= COVERS_NEAR && right >= PSP_W - COVERS_NEAR && bottom >= PSP_H - COVERS_NEAR;
}

static void tell_canvas(void) {
    if (g_canvas_told) return;
    g_canvas_told = 1;
    p_glUniform2f(U.canvas_at, (float)g_canvas_x, (float)g_canvas_y);
    p_glUniform2f(U.canvas_half, (float)g_canvas_w / 2.0f, (float)g_canvas_h / 2.0f);
}

/* Black over the whole canvas. What is drawn next sets again all the state this changes. */
static void draw_dim_veil(void) {
    const float left = (float)-g_canvas_x, top = (float)-g_canvas_y;
    const float right = (float)(g_canvas_w - g_canvas_x), bottom = (float)(g_canvas_h - g_canvas_y);
    const uint32_t colour = (uint32_t)(g_dim_percent * 255 / 100) << 24;
    const gl_vertex quad[6] = {
        {left, top, 0, 1, colour, 0, 0}, {right, top, 0, 1, colour, 0, 0}, {left, bottom, 0, 1, colour, 0, 0},
        {right, top, 0, 1, colour, 0, 0}, {right, bottom, 0, 1, colour, 0, 0}, {left, bottom, 0, 1, colour, 0, 0},
    };
    tell_canvas();
    glEnable(GL_SCISSOR_TEST);
    set_scissor(0, 0, g_canvas_w * g_scale, g_canvas_h * g_scale);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glEnable(GL_BLEND);
    p_glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    p_glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    p_glUniform1i(U.tex_on, 0);
    p_glUniform1i(U.gouraud, 0);
    p_glUniform1i(U.afunc, -1);
    p_glUniform1f(U.zoom, 1.0f);
    p_glUniform3f(U.sfix, 1, 1, 1);
    p_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof quad, quad, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

/* ---- copies of the screen ------------------------------------------------ *
 *
 * For a change of scene, as in some songs, the game copies the screen it showed into its own memory
 * and draws that copy over the new scene as it fades in. Here the picture is on the GPU and never
 * reaches the PSP's video memory, so the copy would be of whatever is left there, a grey screen. A
 * picture of exactly the screen's size, all of one colour, drawn over the whole screen from the
 * game's memory is taken for such a copy: the frame shown last is copied on the GPU when the game's copy changes, and drawn
 * in its place, over the whole canvas, as it covers the whole window. */
#define COPY_NEAR 2.0f
#define COPY_SAMPLES 16
static uint32_t g_front_addr;     /* the frame shown last */
static GLuint g_copy_fbo, g_copy_tex;
static int g_copy_w, g_copy_h;
static uint32_t g_copy_of_addr, g_copy_of_hash;

static int screen_copy_drawn(int prim, int n, const psp_draw_state *st) {
    if (prim != PSP_PRIM_TRIANGLES || n != 6 || !st->tex_enable || st->clear || in_vram(st->tex_addr) || st->tex_w != 512 ||
        st->tex_h != 512)
        return 0;
    float left = g_vb[0].x, right = left, top = g_vb[0].y, bottom = top;
    float u_low = g_vb[0].u, u_high = u_low, v_low = g_vb[0].v, v_high = v_low;
    for (int i = 0; i < 6; i++) {
        if (fabsf(g_vb[i].w - 1.0f) > FLAT_W_NEAR) return 0;
        left = fminf(left, g_vb[i].x);
        right = fmaxf(right, g_vb[i].x);
        top = fminf(top, g_vb[i].y);
        bottom = fmaxf(bottom, g_vb[i].y);
        u_low = fminf(u_low, g_vb[i].u);
        u_high = fmaxf(u_high, g_vb[i].u);
        v_low = fminf(v_low, g_vb[i].v);
        v_high = fmaxf(v_high, g_vb[i].v);
    }
    if (left > COPY_NEAR || top > COPY_NEAR || right < PSP_W - COPY_NEAR || bottom < PSP_H - COPY_NEAR) return 0;
    if (u_low > COPY_NEAR || fabsf(u_high - u_low - (float)PSP_W) > COPY_NEAR || fabsf(v_high - v_low - (float)PSP_H) > COPY_NEAR) return 0;
    /* A song's card is a picture of the same size, drawn the same way. What tells the copy from it is
     * that the video memory it was copied from never gets the GPU's picture, so it is all one colour. */
    const uint32_t first = psp_tex_fetch(st, 0, (int)v_low);
    for (int i = 1; i < COPY_SAMPLES; i++) {
        const int x = (int)(u_low + (u_high - u_low - 1.0f) * (float)(i % 4) / 3.0f);
        const int y = (int)(v_low + (v_high - v_low - 1.0f) * (float)(i / 4) / 3.0f);
        if (psp_tex_fetch(st, x, y) != first) return 0;
    }
    gl_target *front = find_target(g_front_addr);
    gl_texture *e = get_texture(st);
    if (!front || !e) return 0;

    const int w = g_canvas_w * g_scale, h = g_canvas_h * g_scale;
    if (!g_copy_fbo || g_copy_w != w || g_copy_h != h) {
        if (g_copy_fbo) {
            p_glDeleteFramebuffers(1, &g_copy_fbo);
            glDeleteTextures(1, &g_copy_tex);
        }
        g_copy_fbo = make_color_target(&g_copy_tex, w, h, GL_LINEAR);
        g_copy_w = w;
        g_copy_h = h;
        g_copy_of_addr = 0;
    }
    if (g_copy_of_addr != st->tex_addr || g_copy_of_hash != e->data_hash) {
        g_copy_of_addr = st->tex_addr;
        g_copy_of_hash = e->data_hash;
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, front->fbo);
        p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_copy_fbo);
        glDisable(GL_SCISSOR_TEST);
        p_glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        /* What a target keeps in alpha is the game's stencil, not how solid the picture is. */
        p_glBindFramebuffer(GL_FRAMEBUFFER, g_copy_fbo);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        g_bound = NULL;
        bind_target();
        glEnable(GL_SCISSOR_TEST);
        glScissor(g_sc_x, g_sc_y, g_sc_w, g_sc_h);
    }
    glBindTexture(GL_TEXTURE_2D, g_copy_tex);
    p_glUniform2f(U.uvscale, 1.0f / (float)g_canvas_w, 1.0f / (float)g_canvas_h);
    p_glUniform2f(U.uvat, (float)g_canvas_x / (float)g_canvas_w, (float)g_canvas_y / (float)g_canvas_h);
    /* The game's rows count from the bottom of its picture; the canvas keeps its own from the top,
     * and the copy covers all of it. */
    const float middle_x = (left + right) / 2.0f, middle_y = (top + bottom) / 2.0f;
    for (int i = 0; i < 6; i++) {
        const int at_right = g_vb[i].x > middle_x, at_bottom = g_vb[i].y > middle_y;
        g_vb[i].x = g_vb[i].u = at_right ? (float)(g_canvas_w - g_canvas_x) : (float)-g_canvas_x;
        g_vb[i].y = g_vb[i].v = at_bottom ? (float)(g_canvas_h - g_canvas_y) : (float)-g_canvas_y;
    }
    return 1;
}

/* ---- targets in another style ---------------------------------------------
 *
 * The targets the notes fly to come from one 128x128 sheet: a circle, a triangle, a square and a
 * plus in its quarters, and the clock's hand between them. Each target is a model the shape of
 * its own, drawn with no transparency, so another shape cannot be drawn into it. In a note style
 * of its own, a target's model is drawn as a square instead, over the same place, from the
 * style's targets.rgba, whose quarters hold the new shapes in the same order on a clear ground.
 * The hand is left as it is. */
#define TARGETS_SHEET 0xF9253859u
#define HAND_SPAN 40.0f   /* the hand is narrower than this on the sheet; the shapes are wider */

static int read_note_targets(void) {
    if (g_note_targets_read) return g_note_targets != 0;
    g_note_targets_read = 1;
    char path[1200];
    snprintf(path, sizeof path, "%s\\targets.rgba", g_note_dir);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint32_t size[3] = {0, 0, 0};
    uint32_t *pixels = NULL;
    /* Its side in the units its coordinates are given in, then its pixels. */
    if (fread(size, 4, 3, f) == 3 && size[0] && size[0] <= 4096 && size[1] && size[1] <= 4096 && size[2]) {
        const size_t count = (size_t)size[0] * size[1];
        pixels = (uint32_t *)malloc(count * 4);
        if (pixels && fread(pixels, 4, count, f) != count) {
            free(pixels);
            pixels = NULL;
        }
    }
    fclose(f);
    if (!pixels) return 0;
    glGenTextures(1, &g_note_targets);
    glBindTexture(GL_TEXTURE_2D, g_note_targets);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)size[0], (GLsizei)size[1], 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    p_glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    g_note_targets_side = (float)size[2];
    free(pixels);
    fprintf(stderr, "gl: targets of the note style from %s\n", path);
    return 1;
}

/* Takes the n vertices in g_vb; if they are a target in a style of its own, puts six in their
 * place and readies the texture and state for them. */
static int note_target_drawn(int n, const psp_draw_state *st) {
    if (!g_note_dir[0] || !st->tex_enable || g_tex_colours != TARGETS_SHEET || n < 3 || st->tex_w <= 0) return 0;
    float u_low = 1e9f, u_high = -1e9f, r_low = 1e9f, r_high = -1e9f;
    float left = 1e9f, right = -1e9f, top = 1e9f, bottom = -1e9f, z = 0.0f, w = 0.0f;
    for (int i = 0; i < n; i++) {
        const gl_vertex *p = &g_vb[i];
        if (p->w == 0.0f) return 0;
        /* Rows of the sheet from its top: the game counts them from its bottom. */
        const float row = (float)st->tex_h - p->v;
        u_low = fminf(u_low, p->u);
        u_high = fmaxf(u_high, p->u);
        r_low = fminf(r_low, row);
        r_high = fmaxf(r_high, row);
        left = fminf(left, p->x / p->w);
        right = fmaxf(right, p->x / p->w);
        top = fminf(top, p->y / p->w);
        bottom = fmaxf(bottom, p->y / p->w);
        z += p->z;
        w += p->w;
    }
    if (u_high - u_low < HAND_SPAN || r_high - r_low < HAND_SPAN || !read_note_targets()) return 0;
    z /= (float)n;
    w /= (float)n;
    /* Its quarter of the sheet, and where that quarter's corners come on the screen. */
    const float half = (float)st->tex_w / 2.0f;
    const int col = (u_low + u_high) / 2.0f >= half, row = (r_low + r_high) / 2.0f >= half;
    const float qu0 = col * half, qr0 = row * half;
    const float per_u = (right - left) / (u_high - u_low), per_r = (bottom - top) / (r_high - r_low);
    const float x0 = left + (qu0 - u_low) * per_u, x1 = left + (qu0 + half - u_low) * per_u;
    const float y0 = top + (qr0 - r_low) * per_r, y1 = top + (qr0 + half - r_low) * per_r;
    /* The same quarter of the style's sheet, whose rows also count from its bottom. */
    const float side = g_note_targets_side, q = side / 2.0f;
    const float tu0 = col * q, tu1 = tu0 + q, tv0 = side - row * q, tv1 = tv0 - q;
    const float corner[4][4] = {{x0, y0, tu0, tv0}, {x1, y0, tu1, tv0}, {x0, y1, tu0, tv1}, {x1, y1, tu1, tv1}};
    static const int ORDER[6] = {0, 1, 2, 1, 3, 2};
    const gl_vertex like = g_vb[0];
    for (int i = 0; i < 6; i++) {
        const float *c = corner[ORDER[i]];
        g_vb[i] = like;
        g_vb[i].x = c[0] * w;
        g_vb[i].y = c[1] * w;
        g_vb[i].z = z;
        g_vb[i].w = w;
        g_vb[i].u = c[2];
        g_vb[i].v = c[3];
    }
    glBindTexture(GL_TEXTURE_2D, g_note_targets);
    p_glUniform2f(U.uvscale, 1.0f / side, 1.0f / side);
    p_glUniform2f(U.uvat, 0.0f, 0.0f);
    /* Clear around the shape: blended, and what is almost clear not drawn at all, nor its depth. */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    p_glUniform1i(U.afunc, 6);
    p_glUniform1i(U.aref, 8);
    p_glUniform1i(U.amask, 0xFF);
    glDisable(GL_CULL_FACE);
    return 1;
}

static void gl_draw(int prim, const psp_vertex *v, int count, const psp_draw_state *st) {
    if (g_trace_draws == 1 && count > 0) trace_draw(prim, v, count, st);
    if (count <= 0 || st->sc_x1 < st->sc_x0 || st->sc_y1 < st->sc_y0) return;

    bind_target();
    if (g_dim_percent && g_song_frames && !g_dim_done && !st->clear && prim != PSP_PRIM_SPRITES && g_bound && g_bound->shown &&
        /* Notes are models. A flat piece with no depth test, as some songs lay over their whole
         * scene, belongs to the background; the HUD's pieces are single quads. */
        (fabsf(v[0].pw - 1.0f) > FLAT_W_NEAR ? !st->ztest : prim == PSP_PRIM_TRIANGLES && count == 6 && fabsf(v[5].pw - 1.0f) <= FLAT_W_NEAR && !covers_screen(v, count))) {
        g_dim_done = 1;
        draw_dim_veil();
    }
    glEnable(GL_SCISSOR_TEST);
    /* A scissor of the whole screen means the whole canvas, or nothing past it would show. */
    const int whole_screen = st->sc_x0 <= 0 && st->sc_y0 <= 0 && st->sc_x1 >= PSP_W - 1 && st->sc_y1 >= PSP_H - 1;
    if (g_wide && whole_screen)
        set_scissor(0, 0, g_canvas_w * g_scale, g_canvas_h * g_scale);
    else
        set_scissor((st->sc_x0 + g_canvas_x) * g_scale, (st->sc_y0 + g_canvas_y) * g_scale, (st->sc_x1 - st->sc_x0 + 1) * g_scale,
                    (st->sc_y1 - st->sc_y0 + 1) * g_scale);

    if (st->clear)
        glColorMask(st->clear_color ? GL_TRUE : GL_FALSE, st->clear_color ? GL_TRUE : GL_FALSE,
                    st->clear_color ? GL_TRUE : GL_FALSE, st->clear_alpha ? GL_TRUE : GL_FALSE);
    else
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    setup_blend(st);
    setup_depth(st);
    setup_shader(st, setup_texture(st));
    tell_canvas();
    /* Textures the game makes keep their own framing. */
    p_glUniform1f(U.zoom, g_bound && g_bound->shown ? g_zoom : 1.0f);

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
            /* What clears the whole screen clears the whole canvas. */
            if (g_wide && !st->tex_enable && a.x <= 0 && a.y <= 0 && b.x >= PSP_W && b.y >= PSP_H) {
                const float left = (float)-g_canvas_x, top = (float)-g_canvas_y;
                const float right = (float)(g_canvas_w - g_canvas_x), bottom = (float)(g_canvas_h - g_canvas_y);
                tl.x = left; tl.y = top; tr.x = right; tr.y = top;
                bl.x = left; bl.y = bottom; br.x = right; br.y = bottom;
            }
            g_vb[n++] = tl; g_vb[n++] = tr; g_vb[n++] = bl;
            g_vb[n++] = tr; g_vb[n++] = br; g_vb[n++] = bl;
        }
    } else {
        for (int i = 0; i < count; i++) g_vb[n++] = to_gl(&v[i]);
        /* The footer's pieces are placed by the footer, for any shape of window. */
        int footer = 0;
        if (note_target_drawn(n, st)) {
            n = 6;
            mode = GL_TRIANGLES;
            footer = 1;
        }
        if (prim == PSP_PRIM_TRIANGLES && n % 6 == 0 && st->tex_enable && g_tex_colours && g_tex_colours == g_footer_sheet)
            n = footer_draw(n, (float)st->tex_h, &footer);
        /* A copy of the screen is drawn from the GPU's own, and needs nothing more. */
        if (!footer && g_bound && g_bound->shown && screen_copy_drawn(prim, n, st)) footer = 1;
        if (!footer && g_wide && !st->clear && whole_screen && g_bound && g_bound->shown &&
            (prim == PSP_PRIM_TRIANGLES || prim == PSP_PRIM_TRIANGLE_STRIP))
            widen_background_plane(prim, n, st);
        if (!footer && g_wide && !st->clear && whole_screen && g_bound && g_bound->shown) {
            if (!st->tex_enable && n <= 8 && stretch_fade(n)) {
                /* Done. */
            } else if (prim != PSP_PRIM_POINTS && prim != PSP_PRIM_LINES && prim != PSP_PRIM_LINE_STRIP) {
                n = place_flat_piece(prim, n, st);
            }
        }
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

#define MESSAGE_MS 2000u
#define MESSAGE_FOREVER (~0ULL)
#define MESSAGE_LINE 2

static char g_message[256];
static volatile LONG g_message_serial;
static CRITICAL_SECTION g_message_lock;
static volatile LONG g_message_lock_ready;
static volatile int g_message_held;

void psp_render_gl_show_message(const char *utf8) {
    if (InterlockedCompareExchange(&g_message_lock_ready, 1, 0) == 0)
        InitializeCriticalSection(&g_message_lock);
    EnterCriticalSection(&g_message_lock);
    snprintf(g_message, sizeof g_message, "%s", utf8);
    LeaveCriticalSection(&g_message_lock);
    InterlockedIncrement(&g_message_serial);
}

void psp_render_gl_show_held_message(const char *utf8) {
    g_message_held = 1;
    psp_render_gl_show_message(utf8);
}

void psp_render_gl_clear_held_message(void) { g_message_held = 0; }

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
    const int font_h = image_h / 24 > 12 ? image_h / 24 : 12;
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
        /* A dark plate between two lines of the game's teal, as its own windows have. */
        for (int row = 0; row < h; row++) {
            const uint32_t colour = row < MESSAGE_LINE || row >= h - MESSAGE_LINE ? 0x001CD8C9u : 0x001A1E22u;
            uint32_t *line = (uint32_t *)bits + (size_t)row * (size_t)w;
            for (int col = 0; col < w; col++) line[col] = colour;
        }
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
        g_message_until = g_message_held ? MESSAGE_FOREVER : GetTickCount64() + MESSAGE_MS;
        g_message_for_h = 0;
    }
    if (g_message_until == MESSAGE_FOREVER && !g_message_held) g_message_until = 0;
    if (!g_message_until || GetTickCount64() >= g_message_until) return;
    if (g_message_for_h != image_h) {
        g_message_for_h = image_h;
        build_message(image_h);
    }
    if (!g_message_fbo) return;

    const int x = image_x + (image_w - g_message_w) / 2;
    /* Above the bottom band, where nothing is read while it is there. */
    const int y = image_y + image_h / 6;
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, g_message_fbo);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);     /* creating it may have moved this */
    p_glBlitFramebuffer(0, 0, g_message_w, g_message_h, x, y + g_message_h, x + g_message_w, y,
                        GL_COLOR_BUFFER_BIT, GL_NEAREST);
}

static void keep_snapshot(GLuint source_fbo, int source_x, int source_y, int source_w, int source_h) {
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, source_fbo);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_small_fbo);
    p_glBlitFramebuffer(source_x, source_y, source_x + source_w, source_y + source_h, 0, 0, PSP_W, PSP_H, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, g_small_fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, PSP_W, PSP_H, GL_RGBA, GL_UNSIGNED_BYTE, g_snapshot);
    g_have_snapshot = 1;
}

/* Show the buffer at `addr`. The game may have filled it with the CPU rather than
 * by drawing, and then it is only in guest memory. */
static void gl_flip(uint32_t addr, uint32_t stride, int fmt) {
    GLuint source;
    int source_x = 0, source_y = 0, source_w, source_h;
    GLenum filter;

    /* The game draws nothing while the save screen is open, so the last frame stays. */
    if (psp_utility_savedata_open()) return;
    gl_target *t = find_target(addr);
    g_front_addr = addr;
    if (g_trace_draws == 1) {
        g_trace_draws = 0;
        fprintf(stderr, "gl: end of trace %d, shown from %08X\n", g_traces, addr);
    } else if (g_trace_draws == 2) {
        g_trace_draws = 1;
        fprintf(stderr, "gl: trace %d\n", ++g_traces);
    }
    g_flat_last = g_flat_now;
    g_flat_now = 0;
    g_card_last = g_card_now;
    g_card_now = 0;
    g_veil_now = 0;
    g_dialog_last = g_dialog_now;
    g_dialog_now = 0;
    /* Each change is logged, to see what a window was given against what it needed. */
    {
        static float logged_line = -1.0f, logged_width = -1.0f;
        const float width = g_frame_right_now > g_frame_left_now ? g_frame_right_now - g_frame_left_now : 0.0f;
        if (fabsf(g_dialog_line_now - logged_line) > 0.5f || fabsf(width - logged_width) > 0.5f) {
            if (g_dialog_line_now > 0.0f || width > 0.0f)
                fprintf(stderr, "gl: message window %.1f wide, widest line %.1f\n", width, g_dialog_line_now);
            logged_line = g_dialog_line_now;
            logged_width = width;
        }
    }
    g_dialog_line_last = g_dialog_line_now;
    g_dialog_line_now = 0.0f;
    g_frame_left_last = g_frame_left_now;
    g_frame_right_last = g_frame_right_now;
    g_frame_left_now = 1e9f;
    g_frame_right_now = -1e9f;
    g_frame_top_last = g_frame_top_now;
    g_frame_bottom_last = g_frame_bottom_now;
    g_frame_top_now = 1e9f;
    g_frame_bottom_now = -1e9f;
    g_results_last = g_results_now;
    g_results_now = 0;
    g_in_place_last = g_in_place_now;
    g_in_place_now = 0;
    plan_footer();
    g_dim_done = 0;
    if (g_song_frames > 0) g_song_frames--;
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    if (t) {
        source = t->fbo;
        t->shown = 1;
        source_x = g_canvas_x * g_scale;
        source_y = g_canvas_y * g_scale;
        source_w = PSP_W * g_scale;
        source_h = PSP_H * g_scale;
        filter = g_present_linear ? GL_LINEAR : GL_NEAREST;
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

    if (InterlockedExchange(&g_snapshot_asked, 0)) keep_snapshot(source, source_x, source_y, source_w, source_h);

    /* Fit the window, keeping the PSP's shape. Rows are stored top first, and the
     * window's y runs upwards, hence the swapped destination. */
    RECT r;
    GetClientRect(WindowFromDC(g_dc), &r);
    const int cw = r.right, ch = r.bottom;
    /* How much of the canvas the window's shape calls for, as far as the canvas goes. A shape
     * close to the screen's, as 16:9 is, loses a row or two rather than showing bars. What the
     * game filled in memory has no more than the screen, and keeps its bars. */
    const int view = g_wide_view;
    g_zoom = 1.0f;
    g_show_w = PSP_W;
    g_show_h = PSP_H;
    if (g_wide && view && t && cw > 0 && ch > 0) {
        int show_w = PSP_W, show_h = PSP_H;
        if (cw * 5 >= ch * 9) {
            show_w = PSP_H * cw / ch;
            if (show_w > g_canvas_w) show_w = g_canvas_w;
        } else {
            show_h = PSP_W * ch / cw;
            if (show_h > g_canvas_h) show_h = g_canvas_h;
        }
        if (view == 1 && show_h > PSP_H) g_zoom = (float)show_h / (float)PSP_H;
        g_show_w = show_w;
        g_show_h = show_h;
        source_x = (g_canvas_w - show_w) / 2 * g_scale;
        source_y = (g_canvas_h - show_h) / 2 * g_scale;
        source_w = show_w * g_scale;
        source_h = show_h * g_scale;
    }
    if (InterlockedExchange(&g_screenshot_asked, 0)) take_screenshot(source, source_x, source_y, source_w, source_h);
    int w = cw, h = (int)((long long)cw * source_h / source_w);
    if (h > ch) { h = ch; w = (int)((long long)ch * source_w / source_h); }
    const int x0 = (cw - w) / 2, y0 = (ch - h) / 2;
    if (filter == GL_LINEAR) source = halved_towards(source, &source_x, &source_y, &source_w, &source_h, w);

    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, source);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glViewport(0, 0, cw, ch);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    p_glBlitFramebuffer(source_x, source_y, source_x + source_w, source_y + source_h, x0, y0 + h, x0 + w, y0, GL_COLOR_BUFFER_BIT, filter);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    draw_message(x0, y0, w, h);
    if (g_overlay) draw_overlay(x0, y0 + h, h);
    SwapBuffers(g_dc);

    g_bound = NULL;
    g_frame++;
    g_scale_taken_up = 1;
    take_up_scale();
}

/* From any thread. The next present makes the copy, if one comes: a game on the save screen or
 * stuck shows none, and there is then no frame to give. */
static int gl_read_frame(uint32_t *out) {
    g_have_snapshot = 0;
    InterlockedExchange(&g_snapshot_asked, 1);
    for (int waited = 0; !g_have_snapshot && waited < SNAPSHOT_WAIT_MS; waited += 5) Sleep(5);
    if (!g_have_snapshot) return -1;
    memcpy(out, g_snapshot, sizeof g_snapshot);
    return 0;
}

const psp_render_backend psp_render_gl = {
    "gl", gl_init, gl_shutdown, gl_target_changed, gl_draw, gl_noop, gl_noop,
    gl_thread_init, gl_flip, gl_read_frame
};
