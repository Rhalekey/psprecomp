/* psprecomp — render backend selection, and the two backends that need no
 * external dependencies. See docs/RENDERER.md.
 *
 * The software backend is not a fallback. It is the reference: every other
 * backend is correct exactly insofar as it agrees with this one on the same
 * display list, and `tests/test_raster.c` pins its behaviour using synthetic
 * lists with no game data and no GPU. A GPU backend that cannot be diffed
 * against something is a backend whose bugs are indistinguishable from the
 * game's.
 *
 * Per pixel, in order: clear mode short-circuits everything; otherwise vertex
 * colour, texture function, alpha test, depth test, blend, write. Stencil,
 * fog, dithering, colour test and logic ops are not implemented.
 */

#include "psprecomp/render.h"
#include "psprecomp/mem_fast.h"
#include "psprecomp/mem.h"

#include <string.h>

/* ---- shared target state ------------------------------------------------- */

static uint32_t g_fb_addr, g_fb_stride;
static int      g_fb_fmt = 3;
static uint64_t g_pixels;
static uint64_t g_bad_texels;

uint64_t psp_render_pixels(void) { return g_pixels; }
void     psp_render_reset_pixels(void) { g_pixels = 0; g_bad_texels = 0; }
uint64_t psp_render_unsupported_texels(void) { return g_bad_texels; }

/* ---- colour helpers ------------------------------------------------------ */

#define R(c) ((c) & 0xFF)
#define G(c) (((c) >> 8) & 0xFF)
#define B(c) (((c) >> 16) & 0xFF)
#define A(c) ((c) >> 24)

static uint32_t rgba(int r, int g, int b, int a) {
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    if (a > 255) a = 255;
    if (r < 0) r = 0;
    if (g < 0) g = 0;
    if (b < 0) b = 0;
    if (a < 0) a = 0;
    return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
}

static int mul8(int a, int b) { return (a * b + 127) / 255; }

uint32_t psp_color_expand(uint32_t px, int fmt) {
    uint32_t r, g, b, a;
    switch (fmt) {
    case 0:   /* 565 */
        r = px & 0x1F; g = (px >> 5) & 0x3F; b = (px >> 11) & 0x1F;
        return rgba((int)((r << 3) | (r >> 2)), (int)((g << 2) | (g >> 4)),
                    (int)((b << 3) | (b >> 2)), 255);
    case 1:   /* 5551 */
        r = px & 0x1F; g = (px >> 5) & 0x1F; b = (px >> 10) & 0x1F;
        return rgba((int)((r << 3) | (r >> 2)), (int)((g << 3) | (g >> 2)),
                    (int)((b << 3) | (b >> 2)), (px & 0x8000) ? 255 : 0);
    case 2:   /* 4444 */
        r = px & 0xF; g = (px >> 4) & 0xF; b = (px >> 8) & 0xF; a = (px >> 12) & 0xF;
        return rgba((int)((r << 4) | r), (int)((g << 4) | g), (int)((b << 4) | b),
                    (int)((a << 4) | a));
    default:  /* 8888 */
        return px;
    }
}

/* The inverse, for writing 16-bit framebuffers. */
static uint32_t color_pack(uint32_t c, int fmt) {
    switch (fmt) {
    case 0:  return (R(c) >> 3) | ((G(c) >> 2) << 5) | ((B(c) >> 3) << 11);
    case 1:  return (R(c) >> 3) | ((G(c) >> 3) << 5) | ((B(c) >> 3) << 10) | ((A(c) >> 7) << 15);
    case 2:  return (R(c) >> 4) | ((G(c) >> 4) << 4) | ((B(c) >> 4) << 8) | ((A(c) >> 4) << 12);
    default: return c;
    }
}

/* ---- textures ------------------------------------------------------------ */

static const int TEX_BPP[11] = { 16, 16, 16, 32, 4, 8, 16, 32, 4, 8, 8 };

/* Byte offset of texel byte column `xb` in row `y`. A swizzled texture is
 * stored as 16-byte x 8-row blocks, left to right, then top to bottom; that is
 * what makes a row of blocks one contiguous 128-byte run per block. */
static uint32_t tex_offset(const psp_draw_state *st, uint32_t xb, uint32_t y) {
    uint32_t rowb = (uint32_t)st->tex_bw * (uint32_t)TEX_BPP[st->tex_fmt] / 8;
    if (!st->tex_swizzle) return y * rowb + xb;
    uint32_t blocks = rowb / 16;
    return ((y >> 3) * blocks + (xb >> 4)) * 128 + (y & 7) * 16 + (xb & 15);
}

static uint32_t clut_entry(const psp_draw_state *st, uint32_t idx) {
    uint32_t e = ((idx >> st->clut_shift) & (uint32_t)st->clut_mask) |
                 ((uint32_t)st->clut_start << 4);
    if (!st->clut) return 0;
    if (st->clut_fmt == 3) {
        if (e >= 256) return 0;                 /* the CLUT cache is 1 KB */
        const uint8_t *p = st->clut + e * 4;
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }
    if (e >= 512) return 0;
    const uint8_t *p = st->clut + e * 2;
    return psp_color_expand((uint32_t)p[0] | ((uint32_t)p[1] << 8), st->clut_fmt);
}

static int wrap(int c, int size, int clamp) {
    if (clamp) return c < 0 ? 0 : (c >= size ? size - 1 : c);
    return c & (size - 1);                      /* sizes are powers of two */
}

/* DXT1/3/5. The PSP lays a block out with its colour indices first -- one
 * byte per row, two bits per texel -- then the two 565 endpoint colours, the
 * reverse of the desktop order. DXT3 and DXT5 follow the colour half with
 * their alpha half. Blocks run left to right in rows of tex_bw / 4.
 * ponytail: DXT1 is checked against WTF's backgrounds; the DXT5 alpha layout
 * (endpoints in the last two bytes) is not yet seen in a game. */
static uint32_t dxt_texel(const psp_draw_state *st, uint32_t x, uint32_t y) {
    const uint32_t size = st->tex_fmt == PSP_TPSM_DXT1 ? 8u : 16u;
    const uint32_t blk = st->tex_addr + ((y >> 2) * ((uint32_t)st->tex_bw / 4) + (x >> 2)) * size;
    const uint32_t i = (y & 3) * 4 + (x & 3);

    const uint32_t c0 = psp_fast_read16(blk + 4), c1 = psp_fast_read16(blk + 6);
    const uint32_t a = psp_color_expand(c0, 0), b = psp_color_expand(c1, 0);
    uint32_t out;
    switch ((psp_fast_read32(blk) >> (2 * i)) & 3) {
    case 0:  out = a; break;
    case 1:  out = b; break;
    case 2:
        out = c0 > c1 || st->tex_fmt != PSP_TPSM_DXT1
            ? rgba((2 * R(a) + R(b)) / 3, (2 * G(a) + G(b)) / 3, (2 * B(a) + B(b)) / 3, 255)
            : rgba((R(a) + R(b)) / 2, (G(a) + G(b)) / 2, (B(a) + B(b)) / 2, 255);
        break;
    default:
        out = c0 > c1 || st->tex_fmt != PSP_TPSM_DXT1
            ? rgba((R(a) + 2 * R(b)) / 3, (G(a) + 2 * G(b)) / 3, (B(a) + 2 * B(b)) / 3, 255)
            : 0;                                /* DXT1 three-colour mode: transparent */
        break;
    }
    if (st->tex_fmt == PSP_TPSM_DXT3) {
        const uint32_t al = (psp_fast_read16(blk + 8 + (y & 3) * 2) >> ((x & 3) * 4)) & 0xF;
        out = (out & 0x00FFFFFFu) | ((al * 17) << 24);
    } else if (st->tex_fmt == PSP_TPSM_DXT5) {
        const uint64_t bits = (uint64_t)psp_fast_read16(blk + 12) | ((uint64_t)psp_fast_read32(blk + 8) << 16);
        const int a0 = psp_fast_read8(blk + 14), a1 = psp_fast_read8(blk + 15);
        const int k = (int)(bits >> (3 * i)) & 7;
        int al;
        if (k == 0) al = a0;
        else if (k == 1) al = a1;
        else if (a0 > a1) al = ((8 - k) * a0 + (k - 1) * a1) / 7;
        else if (k < 6) al = ((6 - k) * a0 + (k - 1) * a1) / 5;
        else al = k == 6 ? 0 : 255;
        out = (out & 0x00FFFFFFu) | ((uint32_t)al << 24);
    }
    return out;
}

uint32_t psp_tex_fetch(const psp_draw_state *st, int x, int y) {
    if (st->tex_fmt > PSP_TPSM_DXT5 || st->tex_w <= 0 || st->tex_h <= 0 || st->tex_bw <= 0) {
        g_bad_texels++;
        return 0xFFFF00FFu;                     /* nonsense: loud magenta */
    }
    x = wrap(x, st->tex_w, st->tex_clamp_u);
    y = wrap(y, st->tex_h, st->tex_clamp_v);
    if (st->tex_fmt >= PSP_TPSM_DXT1) return dxt_texel(st, (uint32_t)x, (uint32_t)y);

    const int bpp = TEX_BPP[st->tex_fmt];
    const uint32_t at = st->tex_addr + tex_offset(st, (uint32_t)x * (uint32_t)bpp / 8, (uint32_t)y);
    switch (st->tex_fmt) {
    case PSP_TPSM_565: case PSP_TPSM_5551: case PSP_TPSM_4444:
        return psp_color_expand(psp_fast_read16(at), st->tex_fmt);
    case PSP_TPSM_8888:
        return psp_fast_read32(at);
    case PSP_TPSM_T4:
        return clut_entry(st, (psp_fast_read8(at) >> ((x & 1) * 4)) & 0xF);
    case PSP_TPSM_T8:
        return clut_entry(st, psp_fast_read8(at));
    case PSP_TPSM_T16:
        return clut_entry(st, psp_fast_read16(at));
    default:  /* T32 */
        return clut_entry(st, psp_fast_read32(at));
    }
}

/* The texture function combines the texel `t` with the fragment colour `c`. */
static uint32_t tex_combine(const psp_draw_state *st, uint32_t t, uint32_t c) {
    int r, g, b, a;
    const int ta = st->tex_alpha;
    switch (st->tex_func) {
    case 0:   /* modulate */
        r = mul8(R(t), R(c)); g = mul8(G(t), G(c)); b = mul8(B(t), B(c));
        a = ta ? mul8(A(t), A(c)) : (int)A(c);
        break;
    case 1:   /* decal */
        if (ta) {
            r = (R(t) * A(t) + R(c) * (255 - A(t)) + 127) / 255;
            g = (G(t) * A(t) + G(c) * (255 - A(t)) + 127) / 255;
            b = (B(t) * A(t) + B(c) * (255 - A(t)) + 127) / 255;
        } else { r = R(t); g = G(t); b = B(t); }
        a = (int)A(c);
        break;
    case 2: { /* blend: towards the environment colour by the texel */
        uint32_t e = st->tex_env;
        r = (R(c) * (255 - R(t)) + R(e) * R(t) + 127) / 255;
        g = (G(c) * (255 - G(t)) + G(e) * G(t) + 127) / 255;
        b = (B(c) * (255 - B(t)) + B(e) * B(t) + 127) / 255;
        a = ta ? mul8(A(t), A(c)) : (int)A(c);
        break;
    }
    case 3:   /* replace */
        r = R(t); g = G(t); b = B(t);
        a = ta ? (int)A(t) : (int)A(c);
        break;
    default:  /* 4: add */
        r = R(t) + R(c); g = G(t) + G(c); b = B(t) + B(c);
        a = ta ? mul8(A(t), A(c)) : (int)A(c);
        break;
    }
    if (st->tex_double) { r *= 2; g *= 2; b *= 2; }
    return rgba(r, g, b, a);
}

static int alpha_pass(const psp_draw_state *st, uint32_t c) {
    int a = (int)A(c) & st->atest_mask, ref = st->atest_ref & st->atest_mask;
    switch (st->atest_func) {
    case 0:  return 0;
    case 1:  return 1;
    case 2:  return a == ref;
    case 3:  return a != ref;
    case 4:  return a < ref;
    case 5:  return a <= ref;
    case 6:  return a > ref;
    default: return a >= ref;
    }
}

/* Blend factor for one channel. Codes 0 and 1 name the *other* colour: as a
 * source factor they are the destination colour, as a destination factor the
 * source colour. Codes 2-9 are alpha-based for both, and 10 is SFIX/DFIX. */
static int factor(int code, int is_src, int ch, uint32_t s, uint32_t d, uint32_t fix) {
    int other = is_src ? (int)((d >> (ch * 8)) & 0xFF) : (int)((s >> (ch * 8)) & 0xFF);
    switch (code) {
    case 0:  return other;
    case 1:  return 255 - other;
    case 2:  return (int)A(s);
    case 3:  return 255 - (int)A(s);
    case 4:  return (int)A(d);
    case 5:  return 255 - (int)A(d);
    case 6:  return 2 * (int)A(s) > 255 ? 255 : 2 * (int)A(s);
    case 7:  return 255 - 2 * (int)A(s) < 0 ? 0 : 255 - 2 * (int)A(s);
    case 8:  return 2 * (int)A(d) > 255 ? 255 : 2 * (int)A(d);
    case 9:  return 255 - 2 * (int)A(d) < 0 ? 0 : 255 - 2 * (int)A(d);
    default: return (int)((fix >> (ch * 8)) & 0xFF);
    }
}

static uint32_t blend(const psp_draw_state *st, uint32_t s, uint32_t d) {
    int out[3];
    for (int ch = 0; ch < 3; ch++) {
        int sc = (int)((s >> (ch * 8)) & 0xFF), dc = (int)((d >> (ch * 8)) & 0xFF);
        int sv = mul8(sc, factor(st->blend_src, 1, ch, s, d, st->blend_sfix));
        int dv = mul8(dc, factor(st->blend_dst, 0, ch, s, d, st->blend_dfix));
        switch (st->blend_op) {
        case 0:  out[ch] = sv + dv; break;
        case 1:  out[ch] = sv - dv; break;
        case 2:  out[ch] = dv - sv; break;
        case 3:  out[ch] = sc < dc ? sc : dc; break;
        case 4:  out[ch] = sc > dc ? sc : dc; break;
        default: out[ch] = sc > dc ? sc - dc : dc - sc; break;
        }
    }
    /* ponytail: the framebuffer's alpha byte is stencil on hardware; the
     * source alpha is written instead until stencil exists. */
    return rgba(out[0], out[1], out[2], (int)A(s));
}

/* ---- software backend ---------------------------------------------------- */

static int sw_init(int w, int h) { (void)w; (void)h; return 0; }
static void sw_shutdown(void) { }

static void sw_target(uint32_t addr, uint32_t stride, int fmt) {
    g_fb_addr = addr;
    g_fb_stride = stride;
    g_fb_fmt = fmt;
}

static uint32_t fb_read(uint32_t at) {
    return g_fb_fmt == 3 ? psp_fast_read32(at) : psp_color_expand(psp_fast_read16(at), g_fb_fmt);
}

static int depth_pass(int func, int z, int d) {
    switch (func) {
    case 0:  return 0;
    case 1:  return 1;
    case 2:  return z == d;
    case 3:  return z != d;
    case 4:  return z < d;
    case 5:  return z <= d;
    case 6:  return z > d;
    default: return z >= d;
    }
}

/* One fragment through the pipeline. `c` is the interpolated vertex colour,
 * `zf` its depth. */
static void fragment(const psp_draw_state *st, int x, int y, uint32_t c, float u, float v, float zf) {
    if (!g_fb_addr || !g_fb_stride) return;
    if (x < 0 || y < 0 || x >= 480 || y >= 272) return;
    if (x < st->sc_x0 || x > st->sc_x1 || y < st->sc_y0 || y > st->sc_y1) return;

    const uint32_t bytes = g_fb_fmt == 3 ? 4u : 2u;
    const uint32_t at = g_fb_addr + (uint32_t)(y * (int)g_fb_stride + x) * bytes;
    /* Rounded: coplanar layers (WTF draws LED text on its panel at the same
     * depth) must compare equal, and interpolation lands either side of it. */
    const int z = zf < 0.0f ? 0 : (zf > 65535.0f ? 65535 : (int)(zf + 0.5f));
    const uint32_t zat = st->zaddr + (uint32_t)(y * st->zbw + x) * 2u;
    const int have_z = st->zaddr && st->zbw > 0;

    if (st->clear) {
        /* Clear mode writes only the buffers its flags name. A depth-only
         * clear leaves the colour alone. */
        if (st->clear_depth && have_z) psp_fast_write16(zat, (uint16_t)z);
        if (!st->clear_color && !st->clear_alpha) return;
        if (!st->clear_color || !st->clear_alpha) {
            uint32_t d = fb_read(at);
            c = st->clear_color ? (c & 0x00FFFFFFu) | (d & 0xFF000000u)
                                : (d & 0x00FFFFFFu) | (c & 0xFF000000u);
        }
    } else {
        if (st->tex_enable) {
            int tx = (int)u, ty = (int)v;
            if (u < 0 && (float)tx != u) tx--;  /* floor, for negative wraps */
            if (v < 0 && (float)ty != v) ty--;
            c = tex_combine(st, psp_tex_fetch(st, tx, ty), c);
        }
        if (st->atest && !alpha_pass(st, c)) return;
        if (st->ztest && have_z) {
            if (!depth_pass(st->zfunc, z, psp_fast_read16(zat))) return;
            if (st->zwrite) psp_fast_write16(zat, (uint16_t)z);
        }
        if (st->blend) c = blend(st, c, fb_read(at));
    }

    if (bytes == 4) psp_fast_write32(at, c);
    else            psp_fast_write16(at, (uint16_t)color_pack(c, g_fb_fmt));
    g_pixels++;
}

static int edge(const psp_vertex *p, const psp_vertex *q, int x, int y) {
    return (q->x - p->x) * (y - p->y) - (q->y - p->y) * (x - p->x);
}

/* Barycentric fill with integer edge functions, so a shared edge belongs to
 * exactly one triangle: adjacent geometry neither double-draws nor leaves
 * seams. Both windings are filled, and the game decides which to cull: `flip`
 * says this triangle's vertices run the other way round, as every second one in a
 * strip does. Colour is interpolated when the shade model is Gouraud; flat uses
 * the last vertex. */
static void sw_tri(const psp_vertex *a, const psp_vertex *b, const psp_vertex *c,
                   const psp_draw_state *st, int flip) {
    int minx = a->x < b->x ? (a->x < c->x ? a->x : c->x) : (b->x < c->x ? b->x : c->x);
    int maxx = a->x > b->x ? (a->x > c->x ? a->x : c->x) : (b->x > c->x ? b->x : c->x);
    int miny = a->y < b->y ? (a->y < c->y ? a->y : c->y) : (b->y < c->y ? b->y : c->y);
    int maxy = a->y > b->y ? (a->y > c->y ? a->y : c->y) : (b->y > c->y ? b->y : c->y);

    if (a->behind || b->behind || c->behind) return;   /* ponytail: no near-plane clipping */
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx > 479) maxx = 479;
    if (maxy > 271) maxy = 271;
    if (minx < st->sc_x0) minx = st->sc_x0;
    if (miny < st->sc_y0) miny = st->sc_y0;
    if (maxx > st->sc_x1) maxx = st->sc_x1;
    if (maxy > st->sc_y1) maxy = st->sc_y1;
    if (minx > maxx || miny > maxy) return;

    const int area = edge(a, b, c->x, c->y);
    if (area == 0) return;
    /* With y growing downwards, a positive area runs clockwise on screen. */
    const int clockwise = flip ? area < 0 : area > 0;
    if (st->cull == PSP_CULL_CW ? clockwise : st->cull == PSP_CULL_CCW ? !clockwise : 0) return;

    const float inv = 1.0f / (float)area;
    /* Coverage is decided on the integer grid; attributes are sampled at the
     * pixel centre, half a pixel on, as the GE does. Sampled at the corner, a
     * row lands exactly on a texel edge, float error floors it to the texel
     * before, and WTF's LED panels -- dots on every other texel row -- lost
     * whole rows of dots. */
    const float h0 = 0.5f * (float)((b->x - a->x) - (b->y - a->y));
    const float h1 = 0.5f * (float)((c->x - b->x) - (c->y - b->y));
    const float h2 = 0.5f * (float)((a->x - c->x) - (a->y - c->y));

    /* Each edge function changes by a constant per pixel, so they are stepped
     * rather than worked out again, and the pixels of the bounding box outside the
     * triangle cost almost nothing. */
    const int dw0 = a->y - b->y, dw1 = b->y - c->y, dw2 = c->y - a->y;
    int row0 = edge(a, b, minx, miny);   /* weight of c */
    int row1 = edge(b, c, minx, miny);   /* weight of a */
    int row2 = edge(c, a, minx, miny);   /* weight of b */
    for (int y = miny; y <= maxy; y++, row0 += b->x - a->x, row1 += c->x - b->x, row2 += a->x - c->x) {
        int w0 = row0 - dw0, w1 = row1 - dw1, w2 = row2 - dw2;
        for (int x = minx; x <= maxx; x++) {
            w0 += dw0;
            w1 += dw1;
            w2 += dw2;
            if (!((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0)))
                continue;
            float la = ((float)w1 + h1) * inv, lb = ((float)w2 + h2) * inv, lc = ((float)w0 + h0) * inv;
            uint32_t col = c->rgba;
            if (st->gouraud) {
                col = rgba((int)(R(a->rgba) * la + R(b->rgba) * lb + R(c->rgba) * lc + 0.5f),
                           (int)(G(a->rgba) * la + G(b->rgba) * lb + G(c->rgba) * lc + 0.5f),
                           (int)(B(a->rgba) * la + B(b->rgba) * lb + B(c->rgba) * lc + 0.5f),
                           (int)(A(a->rgba) * la + A(b->rgba) * lb + A(c->rgba) * lc + 0.5f));
            }
            fragment(st, x, y, col,
                     a->u * la + b->u * lb + c->u * lc,
                     a->v * la + b->v * lb + c->v * lc,
                     a->z * la + b->z * lb + c->z * lc);
        }
    }
}

/* A sprite is the PSP's 2D primitive: two vertices giving opposite corners of
 * an axis-aligned rectangle, coloured by the second. The far edge is exclusive
 * so adjacent sprites tile without overlapping. Texture coordinates run
 * linearly from one corner to the other, so swapped corners mirror the image. */
static void sw_sprite(const psp_vertex *a, const psp_vertex *b, const psp_draw_state *st) {
    int x0 = a->x < b->x ? a->x : b->x, x1 = a->x > b->x ? a->x : b->x;
    int y0 = a->y < b->y ? a->y : b->y, y1 = a->y > b->y ? a->y : b->y;
    if (x0 == x1 || y0 == y1) return;
    const float du = (b->u - a->u) / (float)(b->x - a->x);
    const float dv = (b->v - a->v) / (float)(b->y - a->y);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            fragment(st, x, y, b->rgba, a->u + ((float)(x - a->x) + 0.5f) * du,
                     a->v + ((float)(y - a->y) + 0.5f) * dv, b->z);
}

static void sw_draw(int prim, const psp_vertex *v, int count, const psp_draw_state *st) {
    switch (prim) {
    case PSP_PRIM_SPRITES:
        for (int i = 0; i + 1 < count; i += 2) sw_sprite(&v[i], &v[i + 1], st);
        break;
    case PSP_PRIM_TRIANGLES:
        for (int i = 0; i + 2 < count; i += 3) sw_tri(&v[i], &v[i + 1], &v[i + 2], st, 0);
        break;
    case PSP_PRIM_TRIANGLE_STRIP:
        for (int i = 0; i + 2 < count; i++) sw_tri(&v[i], &v[i + 1], &v[i + 2], st, i & 1);
        break;
    case PSP_PRIM_TRIANGLE_FAN:
        for (int i = 1; i + 1 < count; i++) sw_tri(&v[0], &v[i], &v[i + 1], st, 0);
        break;
    default:
        break;                       /* points, lines: not yet */
    }
}

static void sw_noop(void) { }

const psp_render_backend psp_render_software = {
    "software", sw_init, sw_shutdown, sw_target, sw_draw, sw_noop, sw_noop
};

/* ---- null backend -------------------------------------------------------- */

/* Draws nothing, and that is the point. During bring-up the question is
 * usually "did the game ask to draw" rather than "does it look right", and the
 * answer is clearer without pixels in the way. */

static int null_init(int w, int h) { (void)w; (void)h; return 0; }
static void null_target(uint32_t a, uint32_t s, int f) { (void)a; (void)s; (void)f; }
static void null_draw(int p, const psp_vertex *v, int n, const psp_draw_state *st) {
    (void)p; (void)v; (void)n; (void)st;
}
static void null_noop(void) { }

const psp_render_backend psp_render_null = {
    "null", null_init, null_noop, null_target, null_draw, null_noop, null_noop
};

/* ---- selection ----------------------------------------------------------- */

static const psp_render_backend *g_backend = &psp_render_software;

const psp_render_backend *psp_render_current(void) { return g_backend; }

int psp_render_select(const char *name) {
    static const psp_render_backend *const all[] = {
        &psp_render_software, &psp_render_null
    };
    if (!name) return -1;
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
        if (strcmp(all[i]->name, name) == 0) { g_backend = all[i]; return 0; }
    }
    return -1;                       /* unknown: keep the current backend */
}
