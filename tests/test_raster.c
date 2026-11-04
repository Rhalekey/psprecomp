/* Rasterizer tests — display list in, pixels out.
 *
 * These use hand-built display lists rather than game data, which is the point:
 * the game cannot yet reach its own draw calls, so waiting for it to render
 * would leave this code completely unmeasured until the last blocker clears.
 * A synthetic list exercises the same path the game will take.
 *
 * The checks are on *where* pixels land, not just how many. A rasterizer that
 * fills the whole screen and one that fills the right rectangle both report a
 * nonzero pixel count, and only one of them is correct.
 */

#include "psprecomp/hle.h"
#include "psprecomp/render.h"
#include "psprecomp/mem.h"
#include "psprecomp/cpu.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...)                                       \
    do {                                                       \
        if (!(cond)) {                                         \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);        \
            printf(__VA_ARGS__);                               \
            printf("\n");                                      \
            failures++;                                        \
        }                                                      \
    } while (0)

#define FB     0x04000000u          /* eDRAM */
#define LIST   0x08800000u
#define VERTS  0x08810000u
#define TEX    0x08820000u
#define CLUT   0x08830000u
#define TEXLIN 0x08840000u

/* VTYPE: 8888 colour, 16-bit position, through mode. */
#define VTYPE_2D  ((7u << 2) | (2u << 7) | (1u << 23))

static uint32_t call(uint32_t nid, uint32_t a0, uint32_t a1, uint32_t a2,
                     uint32_t a3) {
    psp_cpu.r[PSP_REG_A0] = a0;
    psp_cpu.r[PSP_REG_A1] = a1;
    psp_cpu.r[PSP_REG_A2] = a2;
    psp_cpu.r[PSP_REG_A3] = a3;
    psp_hle_call(nid);
    return psp_cpu.r[PSP_REG_V0];
}

static uint32_t *g_list;
static uint32_t g_pc;

static void cmd(uint8_t op, uint32_t arg) {
    psp_write32(LIST + g_pc, ((uint32_t)op << 24) | (arg & 0xFFFFFF));
    g_pc += 4;
}

static void begin_list(void) {
    g_pc = 0;
    (void)g_list;
    /* Addresses do not fit in a 24-bit argument. FBP carries the low 24 bits
     * and FBW smuggles bits 24-31 in its own top byte; VADDR is relative to
     * GE_BASE. Getting this wrong yields a plausible-looking address and
     * silently draws nothing. */
    cmd(0x10, (VERTS >> 8) & 0xFF0000);            /* BASE */
    cmd(0x9C, FB & 0xFFFFFF);                      /* FBP */
    cmd(0x9D, ((FB >> 8) & 0xFF0000) | 480);       /* FBW + address high byte */
    cmd(0x12, VTYPE_2D);                           /* VTYPE */
    cmd(0x01, VERTS & 0xFFFFFF);                   /* VADDR */
}

static void end_list(void) {
    cmd(0x0F, 0);                    /* FINISH */
    cmd(0x0C, 0);                    /* END */
    call(0xAB49E76A, LIST, 0, 0, 0); /* sceGeListEnQueue */
    call(0xB287BD61, 0, 0, 0, 0);    /* sceGeDrawSync */
}

/* One 16-bit-position, 8888-colour vertex. Colour precedes position. */
static void vertex(int idx, int x, int y, uint32_t rgba) {
    uint32_t a = VERTS + (uint32_t)idx * 12;   /* 4 colour + 6 pos, padded to 12 */
    psp_write32(a, rgba);
    psp_write16(a + 4, (uint16_t)x);
    psp_write16(a + 6, (uint16_t)y);
    psp_write16(a + 8, 0);
}

static uint32_t pixel(int x, int y) {
    return psp_read32(FB + (uint32_t)(y * 480 + x) * 4);
}

static void clear_fb(void) {
    for (int y = 0; y < 272; y++)
        for (int x = 0; x < 480; x++)
            psp_write32(FB + (uint32_t)(y * 480 + x) * 4, 0);
}

/* A sprite is the PSP's 2D quad: two vertices, opposite corners. */
static void test_sprite(void) {
    psp_ge_reset();
    clear_fb();
    begin_list();
    vertex(0, 100, 50, 0xFF0000FFu);
    vertex(1, 200, 150, 0xFF0000FFu);
    cmd(0x04, (6u << 16) | 2);       /* PRIM sprites, 2 vertices */
    end_list();

    CHECK(psp_ge_pixels() == 100 * 100, "sprite pixel count: %llu (want 10000)",
          (unsigned long long)psp_ge_pixels());
    CHECK(pixel(150, 100) == 0xFF0000FFu, "sprite interior: 0x%08X", pixel(150, 100));
    CHECK(pixel(99, 100) == 0, "left of sprite should be untouched: 0x%08X", pixel(99, 100));
    CHECK(pixel(150, 49) == 0, "above sprite should be untouched: 0x%08X", pixel(150, 49));
    /* Half-open on the far edge, so adjacent sprites tile without overlapping. */
    CHECK(pixel(200, 100) == 0, "far edge is exclusive: 0x%08X", pixel(200, 100));
}

static void test_triangle(void) {
    psp_ge_reset();
    clear_fb();
    begin_list();
    vertex(0, 10, 10, 0xFF00FF00u);
    vertex(1, 110, 10, 0xFF00FF00u);
    vertex(2, 10, 110, 0xFF00FF00u);
    cmd(0x04, (3u << 16) | 3);       /* PRIM triangles */
    end_list();

    CHECK(psp_ge_pixels() > 4000, "triangle should cover ~5000 px, got %llu",
          (unsigned long long)psp_ge_pixels());
    CHECK(pixel(20, 20) == 0xFF00FF00u, "inside triangle: 0x%08X", pixel(20, 20));
    /* The hypotenuse runs from (110,10) to (10,110); (100,100) is well past it. */
    CHECK(pixel(100, 100) == 0, "outside hypotenuse: 0x%08X", pixel(100, 100));
}

/* Winding must not matter while back-face culling is unimplemented, or half of
 * any real model silently disappears. */
static void test_winding(void) {
    psp_ge_reset();
    clear_fb();
    begin_list();
    vertex(0, 10, 10, 0xFFFFFFFFu);
    vertex(1, 10, 110, 0xFFFFFFFFu);
    vertex(2, 110, 10, 0xFFFFFFFFu);
    cmd(0x04, (3u << 16) | 3);
    end_list();
    CHECK(pixel(20, 20) == 0xFFFFFFFFu, "reversed winding still fills: 0x%08X",
          pixel(20, 20));
}

/* Off-screen geometry must clip, not scribble outside the framebuffer or wrap
 * to the opposite edge. */
static void test_clipping(void) {
    psp_ge_reset();
    clear_fb();
    begin_list();
    vertex(0, -50, -50, 0xFFFF0000u);
    vertex(1, 50, 50, 0xFFFF0000u);
    cmd(0x04, (6u << 16) | 2);
    end_list();

    CHECK(psp_ge_pixels() == 50 * 50, "clipped sprite: %llu (want 2500)",
          (unsigned long long)psp_ge_pixels());
    CHECK(pixel(0, 0) == 0xFFFF0000u, "clipped sprite covers origin");
    CHECK(pixel(479, 271) == 0, "far corner untouched: 0x%08X", pixel(479, 271));
}

/* Transformed geometry goes through world, view and projection, then the
 * viewport and offset the way sceGuViewport(2048, 2048, 480, 272) and
 * sceGuOffset(2048 - 240, 2048 - 136) set them up: clip-space (-1, 1) is the
 * top-left pixel. A world translation of 0.25 must move the quad 60 pixels
 * right, which is what pins the matrices' column order. Texture coordinates
 * are normalised: 0..1 spans the whole texture. */
static uint32_t fbits(float f) { union { float f; uint32_t u; } c; c.f = f; return c.u; }
static void fcmd(uint8_t op, float f) { cmd(op, fbits(f) >> 8); }

#define VTYPE_3D  (3u | (7u << 2) | (3u << 7))      /* float uv, 8888, float xyz */

static void fvertex(int idx, float x, float y, float u, float v) {
    uint32_t a = VERTS + (uint32_t)idx * 24;        /* u v (8) colour (4) x y z (12) */
    psp_write32(a + 0, fbits(u));
    psp_write32(a + 4, fbits(v));
    psp_write32(a + 8, 0xFFFFFFFFu);
    psp_write32(a + 12, fbits(x));
    psp_write32(a + 16, fbits(y));
    psp_write32(a + 20, 0);
}

static void test_transformed_quad(void) {
    for (int y = 0; y < 2; y++)
        for (int x = 0; x < 2; x++)
            psp_write32(TEX + (uint32_t)(y * 2 + x) * 4, 0xFF000000u | (uint32_t)(0x40 * (y * 2 + x + 1)));
    psp_ge_reset();
    clear_fb();
    g_pc = 0;
    cmd(0x10, (VERTS >> 8) & 0xFF0000);
    cmd(0x9C, FB & 0xFFFFFF);
    cmd(0x9D, ((FB >> 8) & 0xFF0000) | 480);
    cmd(0x12, VTYPE_3D);
    cmd(0x01, VERTS & 0xFFFFFF);
    fcmd(0x42, 240.0f);  fcmd(0x43, -136.0f);       /* XSCALE, YSCALE */
    fcmd(0x45, 2048.0f); fcmd(0x46, 2048.0f);       /* XPOS, YPOS */
    cmd(0x4C, (2048 - 240) << 4);                   /* OFFSETX, 12.4 */
    cmd(0x4D, (2048 - 136) << 4);                   /* OFFSETY */
    cmd(0x3A, 0);                                   /* WORLD: identity + (0.25, 0, 0) */
    const float world[12] = { 1, 0, 0,  0, 1, 0,  0, 0, 1,  0.25f, 0, 0 };
    for (int k = 0; k < 12; k++) fcmd(0x3B, world[k]);
    cmd(0x1E, 1);                                   /* 2x2 8888 texture */
    cmd(0xA0, TEX & 0xFFFFFF);
    cmd(0xA8, ((TEX >> 8) & 0xFF0000) | 2);
    cmd(0xB8, (1u << 8) | 1u);
    cmd(0xC3, 3);
    cmd(0xC9, 3);                                   /* replace */
    /* Clip-space square (-0.5, 0.5)-(0.5, -0.5): screen (120, 68)-(360, 204)
     * before the translation, (180, 68)-(420, 204) after. */
    fvertex(0, -0.5f,  0.5f, 0, 0);
    fvertex(1,  0.5f,  0.5f, 1, 0);
    fvertex(2, -0.5f, -0.5f, 0, 1);
    fvertex(3,  0.5f, -0.5f, 1, 1);
    cmd(0x04, (4u << 16) | 4);                      /* triangle strip */
    end_list();

    CHECK(pixel(185, 75) == 0xFF000040u, "top-left quarter shows texel (0,0): 0x%08X", pixel(185, 75));
    CHECK(pixel(415, 75) == 0xFF000080u, "top-right quarter shows texel (1,0): 0x%08X", pixel(415, 75));
    CHECK(pixel(185, 200) == 0xFF0000C0u, "bottom-left quarter shows texel (0,1): 0x%08X", pixel(185, 200));
    CHECK(pixel(175, 136) == 0, "world translation moved the left edge past x=175: 0x%08X", pixel(175, 136));
    CHECK(pixel(425, 136) == 0, "right edge stops before x=425: 0x%08X", pixel(425, 136));
    CHECK(pixel(300, 64) == 0, "top edge is at y=68: 0x%08X", pixel(300, 64));
}

/* Skinned quads: two u8 weights, 8888 colour, float position, so 20 bytes each. */
#define VTYPE_SKINNED ((1u << 9) | (1u << 14) | (7u << 2) | (3u << 7))

static void skinned_vertex(int idx, float x, float y, uint8_t w0, uint8_t w1) {
    const uint32_t a = VERTS + (uint32_t)idx * 20;
    psp_write8(a, w0);
    psp_write8(a + 1, w1);
    psp_write32(a + 4, 0xFFFFFFFFu);
    psp_write_f32(a + 8, x);
    psp_write_f32(a + 12, y);
    psp_write_f32(a + 16, 0);
}

static void draw_skinned_quad(uint8_t w0, uint8_t w1) {
    psp_ge_reset();
    clear_fb();
    g_pc = 0;
    cmd(0x10, (VERTS >> 8) & 0xFF0000);
    cmd(0x9C, FB & 0xFFFFFF);
    cmd(0x9D, ((FB >> 8) & 0xFF0000) | 480);
    cmd(0x12, VTYPE_SKINNED);
    cmd(0x01, VERTS & 0xFFFFFF);
    fcmd(0x42, 240.0f);  fcmd(0x43, -136.0f);
    fcmd(0x45, 2048.0f); fcmd(0x46, 2048.0f);
    cmd(0x4C, (2048 - 240) << 4);
    cmd(0x4D, (2048 - 136) << 4);
    /* Bone 0 moves right by 0.25, bone 1 left by 0.25. */
    const float bones[24] = { 1, 0, 0,  0, 1, 0,  0, 0, 1,   0.25f, 0, 0,
                              1, 0, 0,  0, 1, 0,  0, 0, 1,  -0.25f, 0, 0 };
    cmd(0x2A, 0);
    for (int k = 0; k < 24; k++) fcmd(0x2B, bones[k]);
    skinned_vertex(0, -0.5f,  0.5f, w0, w1);
    skinned_vertex(1,  0.5f,  0.5f, w0, w1);
    skinned_vertex(2, -0.5f, -0.5f, w0, w1);
    skinned_vertex(3,  0.5f, -0.5f, w0, w1);
    cmd(0x04, (4u << 16) | 4);
    end_list();
}

static void test_skinning(void) {
    /* Unskinned, this square covers x 120-360. */
    draw_skinned_quad(0x80, 0);
    CHECK(pixel(185, 75) != 0, "a skinned vertex is drawn");
    CHECK(pixel(175, 136) == 0, "weight 1.0 on bone 0 moves the left edge right: 0x%08X", pixel(175, 136));
    CHECK(pixel(415, 136) != 0, "and the right edge with it");

    draw_skinned_quad(0x40, 0x40);
    CHECK(pixel(125, 136) != 0, "equal weights on opposite bones cancel out");
    CHECK(pixel(365, 136) == 0, "and leave the right edge in place: 0x%08X", pixel(365, 136));
}

static void test_triangle_strip(void) {
    psp_ge_reset();
    clear_fb();
    begin_list();
    vertex(0, 10, 10, 0xFFFFFFFFu);
    vertex(1, 110, 10, 0xFFFFFFFFu);
    vertex(2, 10, 110, 0xFFFFFFFFu);
    vertex(3, 110, 110, 0xFFFFFFFFu);
    cmd(0x04, (4u << 16) | 4);       /* strip: 4 vertices -> 2 triangles */
    end_list();

    /* Two triangles sharing an edge tile the square without a seam. */
    CHECK(pixel(20, 20) == 0xFFFFFFFFu, "strip tri 0: 0x%08X", pixel(20, 20));
    CHECK(pixel(100, 100) == 0xFFFFFFFFu, "strip tri 1: 0x%08X", pixel(100, 100));
}

/* The backend interface itself. The software path is the reference every other
 * backend is diffed against, so selection has to be predictable: an unknown
 * name must not silently leave you rendering into nothing. */
static void test_backend_selection(void) {
    CHECK(psp_render_current() != NULL, "there is always a backend");
    CHECK(strcmp(psp_render_current()->name, "software") == 0,
          "software is the default, got %s", psp_render_current()->name);

    CHECK(psp_render_select("null") == 0, "null backend selectable");
    CHECK(strcmp(psp_render_current()->name, "null") == 0, "null is active");

    /* An unknown name keeps the current backend rather than falling back to
     * something arbitrary -- a typo should not silently change what renders. */
    CHECK(psp_render_select("vulkan-that-does-not-exist") != 0,
          "unknown backend rejected");
    CHECK(strcmp(psp_render_current()->name, "null") == 0,
          "rejected selection leaves the backend alone");

    CHECK(psp_render_select(NULL) != 0, "NULL name rejected");

    /* The null backend must draw nothing: it is what bring-up uses to ask
     * "did the game request a draw" without pixels confusing the answer. */
    psp_ge_reset();
    clear_fb();
    begin_list();
    vertex(0, 10, 10, 0xFFFFFFFFu);
    vertex(1, 60, 60, 0xFFFFFFFFu);
    cmd(0x04, (6u << 16) | 2);
    end_list();
    CHECK(psp_ge_pixels() == 0, "null backend wrote %llu pixels",
          (unsigned long long)psp_ge_pixels());

    CHECK(psp_render_select("software") == 0, "software reselectable");
}

/* ---- textures ------------------------------------------------------------
 *
 * Textured vertices: 16-bit u/v in texels, 8888 colour, 16-bit position,
 * through mode. Components are aligned to their own size, so the layout is
 * u v (4) colour (4) x y z (6), padded to 16. */
#define VTYPE_TEX  (2u | (7u << 2) | (2u << 7) | (1u << 23))

static void tvertex(int idx, int x, int y, int u, int v, uint32_t rgba) {
    uint32_t a = VERTS + (uint32_t)idx * 16;
    psp_write16(a + 0, (uint16_t)u);
    psp_write16(a + 2, (uint16_t)v);
    psp_write32(a + 4, rgba);
    psp_write16(a + 8, (uint16_t)x);
    psp_write16(a + 10, (uint16_t)y);
    psp_write16(a + 12, 0);
}

/* A list with a texture bound: `fmt` at TEX, w x h (powers of two), buffer
 * width w, modulate with texture alpha. */
static void begin_tex_list(int fmt, int logw, int logh, int swizzle) {
    g_pc = 0;
    cmd(0x10, (VERTS >> 8) & 0xFF0000);
    cmd(0x9C, FB & 0xFFFFFF);
    cmd(0x9D, ((FB >> 8) & 0xFF0000) | 480);
    cmd(0x12, VTYPE_TEX);
    cmd(0x01, VERTS & 0xFFFFFF);
    cmd(0x1E, 1);                                  /* TME */
    cmd(0xA0, TEX & 0xFFFFFF);                     /* TBP0 */
    cmd(0xA8, ((TEX >> 8) & 0xFF0000) | (1u << logw)); /* TBW0 */
    cmd(0xB8, ((uint32_t)logh << 8) | (uint32_t)logw); /* TSIZE0 */
    cmd(0xC3, (uint32_t)fmt);                      /* TPSM */
    cmd(0xC2, (uint32_t)swizzle);                  /* TMODE */
    cmd(0xC9, 1u << 8);                            /* TFUNC modulate, RGBA */
}

/* An 8888 4x4 texture whose texel (x, y) is 0xFF0000xy-ish: unique per texel,
 * so a sampling error names the texel it picked. */
static uint32_t texel_id(int x, int y) { return 0xFF000000u | (uint32_t)(y * 16 + x + 1) * 0x010101u; }

static void test_textured_sprite(void) {
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            psp_write32(TEX + (uint32_t)(y * 4 + x) * 4, texel_id(x, y));
    psp_ge_reset();
    clear_fb();
    begin_tex_list(3, 2, 2, 0);
    tvertex(0, 10, 20, 0, 0, 0xFFFFFFFFu);
    tvertex(1, 14, 24, 4, 4, 0xFFFFFFFFu);          /* 1:1 */
    cmd(0x04, (6u << 16) | 2);
    end_list();
    int bad = 0;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            if (pixel(10 + x, 20 + y) != texel_id(x, y)) bad++;
    CHECK(bad == 0, "1:1 textured sprite: %d of 16 texels wrong (0,0)=0x%08X (3,2)=0x%08X",
          bad, pixel(10, 20), pixel(13, 22));

    /* Twice the size: each texel covers 2x2 pixels. */
    psp_ge_reset();
    clear_fb();
    begin_tex_list(3, 2, 2, 0);
    tvertex(0, 0, 0, 0, 0, 0xFFFFFFFFu);
    tvertex(1, 8, 8, 4, 4, 0xFFFFFFFFu);
    cmd(0x04, (6u << 16) | 2);
    end_list();
    CHECK(pixel(5, 3) == texel_id(2, 1), "2x scaled: pixel (5,3) is texel (2,1): 0x%08X", pixel(5, 3));

    /* Modulate: a half-grey vertex colour halves each channel. */
    psp_ge_reset();
    clear_fb();
    begin_tex_list(3, 2, 2, 0);
    psp_write32(TEX, 0xFF80FF40u);
    tvertex(0, 0, 0, 0, 0, 0xFF808080u);
    tvertex(1, 1, 1, 1, 1, 0xFF808080u);
    cmd(0x04, (6u << 16) | 2);
    end_list();
    CHECK(pixel(0, 0) == 0xFF408020u, "modulate by half grey: 0x%08X (want 0xFF408020)", pixel(0, 0));
}

/* Swizzle by moving blocks in storage order, independent of the renderer's
 * address arithmetic: 16-byte x 8-row blocks, row of blocks after row. */
static void swizzle_copy(uint32_t dst, uint32_t src, uint32_t row_bytes, uint32_t rows) {
    uint32_t o = 0;
    for (uint32_t by = 0; by < rows / 8; by++)
        for (uint32_t bx = 0; bx < row_bytes / 16; bx++)
            for (uint32_t r = 0; r < 8; r++)
                for (uint32_t i = 0; i < 16; i++)
                    psp_write8(dst + o++, psp_read8(src + (by * 8 + r) * row_bytes + bx * 16 + i));
}

static void test_clut4_swizzled(void) {
    /* 64x16 CLUT4: 32 bytes per row, so two blocks across and two down. The
     * index of texel (x, y) is (x + y) & 15. */
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 64; x += 2) {
            uint8_t lo = (uint8_t)((x + y) & 15), hi = (uint8_t)((x + 1 + y) & 15);
            psp_write8(TEXLIN + (uint32_t)(y * 32 + x / 2), (uint8_t)(lo | (hi << 4)));
        }
    swizzle_copy(TEX, TEXLIN, 32, 16);
    for (int i = 0; i < 16; i++) psp_write32(CLUT + (uint32_t)i * 4, 0xFF000000u | (uint32_t)(i * 0x0F0E0D));

    psp_ge_reset();
    clear_fb();
    begin_tex_list(4, 6, 4, 1);
    cmd(0xB0, CLUT & 0xFFFFFF);                    /* CBP */
    cmd(0xB1, (CLUT >> 8) & 0xF0000);              /* CBPH */
    cmd(0xC5, (0xFFu << 8) | 3);                   /* CMODE 8888, mask FF */
    cmd(0xC4, 2);                                  /* CLOAD 2 blocks = 16 entries */
    tvertex(0, 0, 0, 0, 0, 0xFFFFFFFFu);
    tvertex(1, 64, 16, 64, 16, 0xFFFFFFFFu);
    cmd(0x04, (6u << 16) | 2);
    end_list();

    int bad = 0, fx = -1, fy = -1;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 64; x++) {
            uint32_t want = 0xFF000000u | (uint32_t)(((x + y) & 15) * 0x0F0E0D);
            if (pixel(x, y) != want) { if (!bad) { fx = x; fy = y; } bad++; }
        }
    CHECK(bad == 0, "swizzled CLUT4: %d of 1024 wrong, first at (%d,%d) = 0x%08X", bad, fx, fy,
          fx >= 0 ? pixel(fx, fy) : 0);

    /* The CLUT is captured at CLOAD: rewriting the palette afterwards does not
     * change a draw that follows without another load. */
    psp_ge_reset();
    clear_fb();
    begin_tex_list(4, 6, 4, 1);
    cmd(0xB0, CLUT & 0xFFFFFF);
    cmd(0xB1, (CLUT >> 8) & 0xF0000);
    cmd(0xC5, (0xFFu << 8) | 3);
    cmd(0xC4, 2);
    end_list();
    psp_write32(CLUT + 4, 0xFF0000FFu);            /* entry 1 -> red, after the load */
    g_pc = 0;
    cmd(0x04, (6u << 16) | 2);
    end_list();
    CHECK(pixel(1, 0) == 0xFF0F0E0Du, "CLUT is copied at load, not read at draw: 0x%08X", pixel(1, 0));
}

static void test_alpha_test_and_blend(void) {
    psp_write32(TEX + 0, 0x00FFFFFFu);             /* texel 0: transparent */
    psp_write32(TEX + 4, 0x800000FFu);             /* texel 1: half-alpha red */

    /* Alpha test GREATER 0 drops the transparent texel, keeps the other. */
    psp_ge_reset();
    for (int i = 0; i < 4; i++) psp_write32(FB + (uint32_t)i * 4, 0xFFFF0000u);   /* blue */
    begin_tex_list(3, 1, 0, 0);
    cmd(0x22, 1);                                  /* ATE */
    cmd(0xDB, (0xFFu << 16) | 6);                  /* ATST > 0 */
    tvertex(0, 0, 0, 0, 0, 0xFFFFFFFFu);
    tvertex(1, 2, 1, 2, 1, 0xFFFFFFFFu);
    cmd(0x04, (6u << 16) | 2);
    end_list();
    CHECK(pixel(0, 0) == 0xFFFF0000u, "alpha test drops alpha-0 texel: 0x%08X", pixel(0, 0));
    CHECK(pixel(1, 0) == 0x800000FFu, "alpha test keeps alpha-128 texel: 0x%08X", pixel(1, 0));

    /* SRC_ALPHA, ONE_MINUS_SRC_ALPHA, ADD: half red over blue. */
    psp_ge_reset();
    for (int i = 0; i < 4; i++) psp_write32(FB + (uint32_t)i * 4, 0xFFFF0000u);
    begin_tex_list(3, 1, 0, 0);
    cmd(0x21, 1);                                  /* ABE */
    cmd(0xDF, (0u << 8) | (3u << 4) | 2u);         /* ALPHA */
    tvertex(0, 1, 0, 1, 0, 0xFFFFFFFFu);
    tvertex(1, 2, 1, 2, 1, 0xFFFFFFFFu);
    cmd(0x04, (6u << 16) | 2);
    end_list();
    uint32_t c = pixel(1, 0);
    int r = (int)(c & 0xFF), b = (int)((c >> 16) & 0xFF), g = (int)((c >> 8) & 0xFF);
    CHECK(r >= 126 && r <= 130 && b >= 125 && b <= 129 && g == 0,
          "half red over blue blends to ~(128,0,127): 0x%08X", c);
}

static void test_clear_mode_and_scissor(void) {
    psp_write32(TEX, 0xFF00FF00u);
    psp_ge_reset();
    clear_fb();
    begin_tex_list(3, 0, 0, 0);
    cmd(0xD3, (7u << 8) | 1);                      /* CLEAR on: texture ignored */
    tvertex(0, 0, 0, 0, 0, 0xFF123456u);
    tvertex(1, 4, 4, 1, 1, 0xFF123456u);
    cmd(0x04, (6u << 16) | 2);
    cmd(0xD3, 0);
    cmd(0xD4, (2u << 10) | 2u);                    /* SCISSOR (2,2)-(3,3) */
    cmd(0xD5, (3u << 10) | 3u);
    tvertex(2, 0, 0, 0, 0, 0xFFFFFFFFu);
    tvertex(3, 8, 8, 1, 1, 0xFFFFFFFFu);
    cmd(0x01, (VERTS + 32) & 0xFFFFFF);
    cmd(0x04, (6u << 16) | 2);
    end_list();
    CHECK(pixel(0, 0) == 0xFF123456u, "clear mode writes the vertex colour: 0x%08X", pixel(0, 0));
    CHECK(pixel(2, 2) == 0xFF00FF00u, "inside the scissor is textured: 0x%08X", pixel(2, 2));
    CHECK(pixel(5, 5) == 0, "outside the scissor is untouched: 0x%08X", pixel(5, 5));
    CHECK(pixel(1, 1) == 0xFF123456u, "scissor kept the clear at (1,1): 0x%08X", pixel(1, 1));

    /* A depth-only clear (bit 10) leaves the colour buffer alone. */
    psp_ge_reset();
    g_pc = 0;
    cmd(0x9C, FB & 0xFFFFFF);
    cmd(0x9D, ((FB >> 8) & 0xFF0000) | 480);
    cmd(0x12, VTYPE_TEX);
    cmd(0x10, (VERTS >> 8) & 0xFF0000);
    cmd(0x01, VERTS & 0xFFFFFF);
    cmd(0xD3, (4u << 8) | 1);
    tvertex(0, 0, 0, 0, 0, 0xFF0000FFu);
    tvertex(1, 4, 4, 1, 1, 0xFF0000FFu);
    cmd(0x04, (6u << 16) | 2);
    end_list();
    CHECK(pixel(0, 0) == 0xFF123456u, "depth-only clear keeps the colour: 0x%08X", pixel(0, 0));
}

/* FBP is an offset into VRAM: games send the pointer relative to eDRAM
 * (sceGuDrawBuffer's 0x88000 is the second buffer), so the high byte is 0.
 * Taken as an absolute address it lands in main RAM. For a module loaded
 * at vaddr 0 that is the game's own code and data. WTF overwrote a jump table
 * this way and every clear went to RAM instead of the screen. */
static void test_framebuffer_is_vram_relative(void) {
    const uint32_t rel = 0x00088000u, ram_probe = rel + (100u * 480u + 150u) * 4u;
    const int ram_ok = psp_mem_ptr(ram_probe, 4) != NULL;
    if (ram_ok) psp_write32(ram_probe, 0x12345678u);
    psp_ge_reset();
    g_pc = 0;
    cmd(0x10, (VERTS >> 8) & 0xFF0000);
    cmd(0x9C, rel);                       /* FBP, relative */
    cmd(0x9D, 480);                       /* FBW, high byte 0 */
    cmd(0x12, VTYPE_2D);
    cmd(0x01, VERTS & 0xFFFFFF);
    vertex(0, 100, 50, 0xFF00FF00u);
    vertex(1, 200, 150, 0xFF00FF00u);
    cmd(0x04, (6u << 16) | 2);
    end_list();

    uint32_t got = psp_read32(PSP_VRAM_BASE + ram_probe);
    CHECK(got == 0xFF00FF00u, "relative FBP draws into VRAM at 0x%08X: 0x%08X",
          PSP_VRAM_BASE + ram_probe, got);
    if (ram_ok)
        CHECK(psp_read32(ram_probe) == 0x12345678u,
              "relative FBP must not write main RAM at 0x%08X: 0x%08X",
              ram_probe, psp_read32(ram_probe));
}

/* ---- DXT, depth, sampling, vertex flow, lighting ------------------------ */

/* One DXT1 block, PSP layout: the 2-bit indices first (a byte per row), then
 * the two 565 endpoints. Endpoint 0 > endpoint 1 selects four-colour mode, so
 * indices 2 and 3 are the 2/3 and 1/3 blends. */
static void test_dxt1(void) {
    psp_write32(TEX, 0xE4E4E4E4u);                 /* every row: 0, 1, 2, 3 */
    psp_write16(TEX + 4, 0xF800);                  /* endpoint 0: blue */
    psp_write16(TEX + 6, 0x001F);                  /* endpoint 1: red */
    psp_ge_reset();
    clear_fb();
    begin_tex_list(8, 2, 2, 0);                    /* DXT1, 4x4 */
    tvertex(0, 0, 0, 0, 0, 0xFFFFFFFFu);
    tvertex(1, 4, 4, 4, 4, 0xFFFFFFFFu);
    cmd(0x04, (6u << 16) | 2);
    end_list();
    CHECK(pixel(0, 0) == 0xFFFF0000u, "DXT1 index 0 is endpoint 0 (blue): 0x%08X", pixel(0, 0));
    CHECK(pixel(1, 0) == 0xFF0000FFu, "DXT1 index 1 is endpoint 1 (red): 0x%08X", pixel(1, 0));
    CHECK(pixel(2, 0) == 0xFFAA0055u, "DXT1 index 2 is 2/3 endpoint 0: 0x%08X", pixel(2, 0));
    CHECK(pixel(3, 3) == 0xFF5500AAu, "DXT1 index 3 is 1/3 endpoint 0: 0x%08X", pixel(3, 3));
    CHECK(psp_render_unsupported_texels() == 0, "no texel was undecodable");
}

/* Through-mode sprites with a depth: GEQUAL keeps the nearer (larger) one,
 * and an equal depth passes, so coplanar layers draw in order. */
static void zsprite(int idx, int x0, int y0, int x1, int y1, int z, uint32_t rgba) {
    for (int k = 0; k < 2; k++) {
        uint32_t a = VERTS + (uint32_t)(idx + k) * 12;
        psp_write32(a, rgba);
        psp_write16(a + 4, (uint16_t)(k ? x1 : x0));
        psp_write16(a + 6, (uint16_t)(k ? y1 : y0));
        psp_write16(a + 8, (uint16_t)z);
    }
}

static void test_depth(void) {
    const uint32_t ZB = 0x00110000u;               /* VRAM-relative, like FBP */
    for (uint32_t i = 0; i < 16u * 480u; i++) psp_write16(PSP_VRAM_BASE + ZB + i * 2, 0);
    psp_ge_reset();
    clear_fb();
    begin_list();
    cmd(0x9E, ZB & 0xFFFFFF);                      /* ZBP */
    cmd(0x9F, 480);                                /* ZBW */
    cmd(0x23, 1);                                  /* ZTE */
    cmd(0xDE, 7);                                  /* ZTST GEQUAL */
    zsprite(0, 0, 0, 8, 8, 100, 0xFF0000FFu);
    zsprite(2, 4, 0, 12, 8, 50, 0xFF00FF00u);      /* farther: hidden where they overlap */
    zsprite(4, 0, 4, 4, 8, 100, 0xFFFF0000u);      /* equal: drawn */
    cmd(0x04, (6u << 16) | 6);
    end_list();
    CHECK(pixel(2, 2) == 0xFF0000FFu, "nearer sprite drawn: 0x%08X", pixel(2, 2));
    CHECK(pixel(6, 2) == 0xFF0000FFu, "farther sprite hidden behind it: 0x%08X", pixel(6, 2));
    CHECK(pixel(10, 2) == 0xFF00FF00u, "farther sprite drawn where uncovered: 0x%08X", pixel(10, 2));
    CHECK(pixel(2, 6) == 0xFFFF0000u, "equal depth passes GEQUAL: 0x%08X", pixel(2, 6));
}

/* Attributes are sampled at pixel centres. This is WTF's LED panel: a quad
 * 243 pixels wide mapping 244 texels, 16 rows 1:1, as two triangles. At the
 * pixel corner the v of row 2 came out 1.99999 and floored to row 1, and the
 * panel -- lit dots on every other row -- lost whole rows. */
static void test_samples_at_pixel_centres(void) {
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 256; x++)
            psp_write32(TEX + (uint32_t)(y * 256 + x) * 4, 0xFF000000u | (uint32_t)(y + 1));
    psp_ge_reset();
    clear_fb();
    begin_tex_list(3, 8, 4, 0);                    /* 8888, 256x16 */
    cmd(0xC9, (1u << 8) | 3);                      /* replace */
    tvertex(0, 118, 159, 0, 0, 0xFFFFFFFFu);
    tvertex(1, 361, 159, 244, 0, 0xFFFFFFFFu);
    tvertex(2, 118, 175, 0, 16, 0xFFFFFFFFu);
    tvertex(3, 361, 175, 244, 16, 0xFFFFFFFFu);
    tvertex(4, 118, 175, 0, 16, 0xFFFFFFFFu);
    tvertex(5, 361, 159, 244, 0, 0xFFFFFFFFu);
    cmd(0x04, (3u << 16) | 6);
    end_list();
    int bad = 0, first = -1;
    for (int y = 0; y < 16; y++)
        for (int x = 120; x < 360; x += 7)
            if (pixel(x, 159 + y) != (0xFF000000u | (uint32_t)(y + 1))) { bad++; if (first < 0) first = y; }
    CHECK(bad == 0, "every row samples its own texel row: %d wrong, first in row %d", bad, first);
}

/* A PRIM leaves the vertex pointer after what it used, so a second PRIM with
 * no VADDR continues from there; with an index type, vertices come through
 * the index list instead and IADDR advances. */
static void test_vertex_flow(void) {
    psp_ge_reset();
    clear_fb();
    begin_list();
    vertex(0, 0, 0, 0xFF0000FFu);
    vertex(1, 4, 4, 0xFF0000FFu);
    vertex(2, 10, 0, 0xFF00FF00u);
    vertex(3, 14, 4, 0xFF00FF00u);
    cmd(0x04, (6u << 16) | 2);
    cmd(0x04, (6u << 16) | 2);                     /* no new VADDR */
    psp_write8(VERTS + 0x400, 3);                  /* indices: vertex 2 then 3 */
    psp_write8(VERTS + 0x401, 2);
    psp_write8(VERTS + 0x402, 0);
    psp_write8(VERTS + 0x403, 1);
    cmd(0x12, VTYPE_2D | (1u << 11));              /* 8-bit indices */
    cmd(0x01, VERTS & 0xFFFFFF);
    cmd(0x02, (VERTS + 0x400) & 0xFFFFFF);         /* IADDR */
    cmd(0x04, (6u << 16) | 2);                     /* corners 3, 2: still the green box */
    cmd(0x04, (6u << 16) | 2);                     /* IADDR moved on: 0, 1, the red box */
    end_list();
    CHECK(pixel(2, 2) == 0xFF0000FFu, "first sprite: 0x%08X", pixel(2, 2));
    CHECK(pixel(12, 2) == 0xFF00FF00u, "second PRIM continued from the vertex pointer: 0x%08X", pixel(12, 2));
    CHECK(psp_ge_pixels() == 4 * 16, "indexed sprites drew the same boxes again: %llu pixels",
          (unsigned long long)psp_ge_pixels());
}

/* One directional light along +z on a face whose normal is +z: the face takes
 * the full diffuse colour. Turned to +x, N.L is 0 and only ambient (none)
 * remains. Lights are in world space and act only on transformed vertices. */
static void lit_quad(float lx, float lz) {
    psp_ge_reset();
    clear_fb();
    g_pc = 0;
    cmd(0x10, (VERTS >> 8) & 0xFF0000);
    cmd(0x9C, FB & 0xFFFFFF);
    cmd(0x9D, ((FB >> 8) & 0xFF0000) | 480);
    cmd(0x12, (3u << 5) | (3u << 7));              /* float normal, float position */
    cmd(0x01, VERTS & 0xFFFFFF);
    fcmd(0x42, 240.0f);  fcmd(0x43, -136.0f);
    fcmd(0x45, 2048.0f); fcmd(0x46, 2048.0f);
    cmd(0x4C, (2048 - 240) << 4);
    cmd(0x4D, (2048 - 136) << 4);
    cmd(0x17, 1);                                  /* LTE */
    cmd(0x18, 1);                                  /* light 0 on */
    cmd(0x5F, 0);                                  /* directional, diffuse only */
    fcmd(0x63, lx); fcmd(0x64, 0); fcmd(0x65, lz);
    cmd(0x90, 0xFFFFFF);                           /* light 0 diffuse: white */
    cmd(0x56, 0x0000FF);                           /* material diffuse: red */
    cmd(0x5C, 0);                                  /* no ambient light */
    const float q[4][2] = { { -0.5f, 0.5f }, { 0.5f, 0.5f }, { -0.5f, -0.5f }, { 0.5f, -0.5f } };
    for (int i = 0; i < 4; i++) {
        uint32_t a = VERTS + (uint32_t)i * 24;
        psp_write32(a + 0, 0); psp_write32(a + 4, 0); psp_write32(a + 8, fbits(1.0f));
        psp_write32(a + 12, fbits(q[i][0])); psp_write32(a + 16, fbits(q[i][1])); psp_write32(a + 20, 0);
    }
    cmd(0x04, (4u << 16) | 4);
    end_list();
}

static void test_lighting(void) {
    lit_quad(0, 1);
    CHECK(pixel(240, 136) == 0xFF0000FFu, "light along the normal: full diffuse: 0x%08X", pixel(240, 136));
    lit_quad(1, 0);
    CHECK(pixel(240, 136) == 0xFF000000u, "light across the face: no diffuse: 0x%08X", pixel(240, 136));
}

int main(void) {
    if (psp_mem_init() != 0) { printf("memory init failed\n"); return 1; }
    psp_cpu_reset();
    psp_hle_init();
    psp_cpu.r[PSP_REG_SP] = 0x08F00000u;

    test_sprite();
    test_triangle();
    test_winding();
    test_clipping();
    test_transformed_quad();
    test_skinning();
    test_triangle_strip();
    test_framebuffer_is_vram_relative();
    test_textured_sprite();
    test_clut4_swizzled();
    test_alpha_test_and_blend();
    test_clear_mode_and_scissor();
    test_dxt1();
    test_depth();
    test_samples_at_pixel_centres();
    test_vertex_flow();
    test_lighting();
    test_backend_selection();

    /* The same results when the GE runs on its own thread. */
    psp_ge_set_async(1);
    test_sprite();
    test_transformed_quad();
    test_skinning();
    test_depth();
    psp_ge_set_async(0);

    psp_mem_free();
    printf(failures ? "raster: %d failure(s)\n" : "raster: all tests passed\n", failures);
    return failures ? 1 : 0;
}
