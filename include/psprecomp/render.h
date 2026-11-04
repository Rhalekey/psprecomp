/* psprecomp -- render backend interface. See docs/RENDERER.md.
 *
 * Display-list *interpretation* has one implementation (src/hle/ge.c);
 * *presentation* is pluggable. The split matters because the software
 * rasterizer is the oracle every other backend is checked against, and it has
 * to keep working on a machine with no GPU.
 *
 * Primitives arrive assembled -- vertex format decoding happens once, in the
 * interpreter, rather than being duplicated (and mis-duplicated) per backend.
 * Texel decoding is shared the same way: psp_tex_fetch() is the one place that
 * knows the texture formats, swizzling and CLUTs.
 */
#ifndef PSPRECOMP_RENDER_H
#define PSPRECOMP_RENDER_H

#include <stdint.h>

/* A vertex after format decoding: screen space, colour resolved, texture
 * coordinates in texels (through mode addresses texels directly). Colours
 * here and below are 0xAABBGGRR, the GE's own byte order. */
typedef struct {
    int      x, y;
    float    z;              /* depth, 0..65535 */
    uint32_t rgba;
    float    u, v;
    int      behind;         /* behind the eye: triangles using it are dropped */
    /* The same position before the division by w, for a backend that clips and
     * interpolates with perspective: x and y are pixels times w, z is depth times
     * w. In through mode w is 1. */
    float    px, py, pz, pw;
} psp_vertex;

/* GE primitive types, from the PRIM argument's type field. */
enum {
    PSP_PRIM_POINTS = 0,
    PSP_PRIM_LINES,
    PSP_PRIM_LINE_STRIP,
    PSP_PRIM_TRIANGLES,
    PSP_PRIM_TRIANGLE_STRIP,
    PSP_PRIM_TRIANGLE_FAN,
    PSP_PRIM_SPRITES
};

/* GE texture / CLUT formats (TPSM, CMODE). 0-3 are also the framebuffer and
 * display formats. */
enum {
    PSP_TPSM_565 = 0, PSP_TPSM_5551, PSP_TPSM_4444, PSP_TPSM_8888,
    PSP_TPSM_T4, PSP_TPSM_T8, PSP_TPSM_T16, PSP_TPSM_T32,
    PSP_TPSM_DXT1, PSP_TPSM_DXT3, PSP_TPSM_DXT5
};

/* Everything about the GE's state that changes what a primitive's pixels
 * become. Filled by the interpreter from the state commands; read by the
 * backend. Field comments name the command each comes from. */
typedef struct {
    int      clear;          /* CLEAR bit 0: write vertex colour, nothing else */
    int      clear_color;    /* CLEAR bit 8: ...to RGB */
    int      clear_alpha;    /* CLEAR bit 9: ...to alpha (stencil) */
    int      clear_depth;    /* CLEAR bit 10: ...to depth */
    int      gouraud;        /* SHADE */

    int      tex_enable;     /* TME */
    uint32_t tex_addr;       /* TBP0 + TBW0 high byte */
    int      tex_bw;         /* TBW0: buffer width, texels */
    int      tex_w, tex_h;   /* TSIZE0, as sizes not logs */
    int      tex_fmt;        /* TPSM */
    int      tex_swizzle;    /* TMODE bit 0 */
    int      tex_clamp_u, tex_clamp_v;   /* TWRAP */
    int      tex_func;       /* TFUNC bits 0-2 */
    int      tex_alpha;      /* TFUNC bit 8: texture alpha is used */
    int      tex_double;     /* TFUNC bit 16: colour doubling */
    int      tex_min_linear, tex_mag_linear;   /* TFLT, without mipmaps */
    uint32_t tex_env;        /* TEC, for the blend function */

    const uint8_t *clut;     /* bytes captured at the last CLOAD */
    int      clut_fmt;       /* CMODE bits 0-1 */
    int      clut_shift;     /* CMODE bits 2-6 */
    int      clut_mask;      /* CMODE bits 8-15 */
    int      clut_start;     /* CMODE bits 16-20, in units of 16 entries */

    int      blend;          /* ABE */
    int      blend_src, blend_dst, blend_op;   /* ALPHA */
    uint32_t blend_sfix, blend_dfix;           /* SFIX, DFIX */

    int      atest;          /* ATE */
    int      atest_func, atest_ref, atest_mask; /* ATST */

    int      sc_x0, sc_y0, sc_x1, sc_y1;        /* SCISSOR1/2, inclusive */

    int      ztest;          /* ZTE */
    int      zfunc;          /* ZTST: 0 never .. 7 greater-or-equal */
    int      zwrite;         /* !ZMSK */
    uint32_t zaddr;          /* ZBP/ZBW, in VRAM; 16 bits per pixel */
    int      zbw;            /* ZBW: width in pixels */
    int cull;            /* PSP_CULL_*: which triangles a transformed draw discards */
} psp_draw_state;

#define PSP_CULL_NONE 0
#define PSP_CULL_CW   1     /* as seen on screen */
#define PSP_CULL_CCW  2

typedef struct {
    const char *name;

    int  (*init)(int width, int height);
    void (*shutdown)(void);

    /* GE_FBP / GE_FBW: the framebuffer being drawn into. */
    void (*set_target)(uint32_t addr, uint32_t stride, int fmt);

    /* One assembled primitive. `count` vertices, already in screen space,
     * drawn with state `st`. */
    void (*draw)(int prim, const psp_vertex *v, int count, const psp_draw_state *st);

    /* End of a display list -- a natural point to flush batched work. */
    void (*finish)(void);

    /* sceDisplaySetFrameBuf -- show what has accumulated. */
    void (*present)(void);

    /* Optional, for a backend drawing on the GE's own thread. thread_init runs
     * there before anything else; a failure falls back to the software backend.
     * flip shows the buffer at `addr`, in order with the lists around it. A
     * backend whose pixels never reach guest VRAM copies a frame into `out`,
     * 480x272 of 0xAABBGGRR, and returns 0. */
    int  (*thread_init)(void);
    void (*flip)(uint32_t addr, uint32_t stride, int fmt);
    int  (*read_frame)(uint32_t *out);
} psp_render_backend;

/* Select a backend by name ("software", "null", ...). Returns 0 on success,
 * -1 if the name is unknown, leaving the current backend in place. */
int psp_render_select(const char *name);

/* The active backend. Never NULL -- defaults to software. */
const psp_render_backend *psp_render_current(void);

/* Backends provided by the runtime. */
extern const psp_render_backend psp_render_software;
extern const psp_render_backend psp_render_null;
/* OpenGL 3.3, with real threads on Windows. Needs the host window and the GE's
 * thread, so select it before psp_ge_set_async(1). */
extern const psp_render_backend psp_render_gl;
/* How many times the PSP's resolution targets are drawn at. Set it first. */
void psp_render_gl_set_scale(int scale);
/* Show frames per second and the slowest frame in a corner of the window. */
void psp_render_gl_set_overlay(int on);
/* Textures decoded so far and the real time that took, in microseconds. */
void psp_render_gl_texture_stats(uint64_t *count, uint64_t *total_us, uint64_t *worst_us);

/* Pixels written by the active backend -- the proof of life during bring-up. */
uint64_t psp_render_pixels(void);
void     psp_render_reset_pixels(void);

/* One pixel in format 0-3 (565, 5551, 4444, 8888) as 0xAABBGGRR. The 16-bit
 * formats replicate their high bits into the low ones. */
uint32_t psp_color_expand(uint32_t px, int fmt);

/* The texel at integer coordinates (x, y), wrapped or clamped, as 0xAABBGGRR.
 * Anything undecodable returns opaque magenta and is counted. */
uint32_t psp_tex_fetch(const psp_draw_state *st, int x, int y);
uint64_t psp_render_unsupported_texels(void);

#endif
