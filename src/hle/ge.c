/* psprecomp — sceGe_user.
 *
 * The GE is the PSP's GPU. It is not driven by function calls: user code builds
 * a **display list** — an array of 32-bit words, each an 8-bit command and 24
 * bits of argument — and hands the GE a pointer plus a *stall address*. The GE
 * consumes commands up to the stall, and the CPU moves the stall forward as it
 * writes more. That producer/consumer arrangement is the whole API.
 *
 * So `sceGu*` (the list-building library) is ordinary user code and gets
 * recompiled like anything else. Only list *execution* is emulated, and that is
 * this file.
 *
 * ## What this does and does not do
 *
 * It walks the list, follows control flow (JUMP/CALL/RET/END/FINISH), tracks
 * the state commands that matter, decodes and transforms vertices, and hands
 * assembled primitives plus a psp_draw_state to the render backend, which
 * rasterizes them (src/render.c). See the rasterizer section below for what
 * is and is not decoded.
 *
 * psp_ge_dump_stats() reports what the game asked for, including every
 * command not decoded, by opcode, with the last argument it carried. During
 * bring-up that census answers "what is the game drawing, and with what state"
 * before any pixel is trusted.
 */

#include "psprecomp/hle.h"
#include "psprecomp/dispatch.h"
#include "psprecomp/render.h"
#include "psprecomp/mem_fast.h"
#include "psprecomp/window.h"
#include "ge_thread.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Display-list opcodes. Only the ones the walk needs to be correct about are
 * named; everything else is counted rather than guessed at, because a
 * misidentified state command silently changes rendering. */
#define GE_NOP          0x00
#define GE_VADDR        0x01
#define GE_IADDR        0x02
#define GE_PRIM         0x04
#define GE_BEZIER       0x05
#define GE_SPLINE       0x06
#define GE_JUMP         0x08
#define GE_BJUMP        0x09
#define GE_CALL         0x0A
#define GE_RET          0x0B
#define GE_END          0x0C
#define GE_SIGNAL       0x0E
#define GE_FINISH       0x0F
#define GE_BASE         0x10
#define GE_VTYPE        0x12
#define GE_OFFSET_ADDR  0x13
#define GE_ORIGIN_ADDR  0x14
#define GE_TME          0x1E
#define GE_LTE          0x17
#define GE_ABE          0x21
#define GE_ATE          0x22
#define GE_ZTE          0x23
#define GE_ZSCALE       0x44
#define GE_ZPOS         0x47
#define GE_RNORM        0x51
#define GE_CMAT         0x53
#define GE_EMC          0x54
#define GE_DMC          0x56
#define GE_SMC          0x57
#define GE_SPOW         0x5B
#define GE_ALC          0x5C
#define GE_ALA          0x5D
#define GE_ZBP          0x9E
#define GE_ZBW          0x9F
#define GE_ZTST         0xDE
#define GE_ZMSK         0xE7
#define GE_TFLT         0xC6    /* texture filters */
#define GE_BCE          0x1D    /* back-face culling on or off */
#define GE_CULL         0x9B    /* which way round a front face runs */
#define GE_BOFS         0x2A
#define GE_BONE         0x2B
#define GE_WMS          0x3A
#define GE_WORLD        0x3B
#define GE_VMS          0x3C
#define GE_VIEW         0x3D
#define GE_PMS          0x3E
#define GE_PROJ         0x3F
#define GE_XSCALE       0x42
#define GE_YSCALE       0x43
#define GE_XPOS         0x45
#define GE_YPOS         0x46
#define GE_USCALE       0x48
#define GE_VSCALE       0x49
#define GE_UOFFSET      0x4A
#define GE_VOFFSET      0x4B
#define GE_OFFSETX      0x4C
#define GE_OFFSETY      0x4D
#define GE_SHADE        0x50
#define GE_AMC          0x55
#define GE_AMA          0x58
#define GE_FBP          0x9C
#define GE_FBW          0x9D
#define GE_TBP0         0xA0
#define GE_TBW0         0xA8
#define GE_CBP          0xB0
#define GE_CBPH         0xB1
#define GE_TSIZE0       0xB8
#define GE_TMODE        0xC2
#define GE_TPSM         0xC3
#define GE_CLOAD        0xC4
#define GE_CMODE        0xC5
#define GE_TWRAP        0xC7
#define GE_TFUNC        0xC9
#define GE_TEC          0xCA
#define GE_PSM          0xD2
#define GE_CLEAR        0xD3
#define GE_SCISSOR1     0xD4
#define GE_SCISSOR2     0xD5
#define GE_ATST         0xDB
#define GE_ALPHA        0xDF
#define GE_SFIX         0xE0
#define GE_DFIX         0xE1

#define MAX_QUEUES 8
#define GE_STACK   8

/* Primitive types, from the PRIM argument's type field. */
static const char *const PRIM_NAME[8] = {
    "points", "lines", "line-strip", "triangles",
    "triangle-strip", "triangle-fan", "sprites", "?"
};

typedef struct {
    uint32_t id;
    uint32_t list;      /* current read pointer */
    uint32_t stall;     /* stop before this address; 0 means "no stall" */
    uint32_t base;      /* GE_BASE: high bits for addresses */
    uint32_t origin;
    int      used;
    int      done;
    uint32_t cbid;      /* sceGeSetCallback's id, as passed to the enqueue */
    int      queued;    /* waiting for the GE thread */
} ge_queue;

static ge_queue g_queue[MAX_QUEUES];

/* ---- callbacks ------------------------------------------------------------
 *
 * The GE tells the CPU about a list through interrupts: FINISH and SIGNAL each
 * run a callback set with sceGeSetCallback. A game can depend on that entirely,
 * polling a flag its finish callback sets without ever calling a sync function.
 *
 * A callback must run on a guest thread, and may be raised before the game has
 * even seen the list's id. It is queued and delivered at the next wait, as a real
 * interrupt would arrive after the call. The GE thread raises and a guest thread
 * delivers, so the queue is locked. */
#define MAX_GE_CALLBACKS 16
#define MAX_GE_PENDING   64

typedef struct {
    int used;
    uint32_t signal_fn, signal_arg, finish_fn, finish_arg;
} ge_callback;

typedef struct { uint32_t fn, a0, a1; } ge_pending;

static ge_callback g_ge_cb[MAX_GE_CALLBACKS];
static ge_pending  g_ge_pending[MAX_GE_PENDING];
static int         g_ge_npending;
static int         g_ge_delivering;
static uint64_t    g_ge_delivered, g_ge_dropped;

uint64_t psp_ge_callbacks_delivered(void) { return g_ge_delivered; }

static void ge_raise(const ge_queue *q, int is_signal, uint32_t value) {
    if (q->cbid >= MAX_GE_CALLBACKS || !g_ge_cb[q->cbid].used) return;
    const ge_callback *c = &g_ge_cb[q->cbid];
    const uint32_t fn = is_signal ? c->signal_fn : c->finish_fn;
    if (!fn) return;
    ge_lock();
    if (g_ge_npending == MAX_GE_PENDING) {
        g_ge_dropped++;
    } else {
        ge_pending *p = &g_ge_pending[g_ge_npending++];
        p->fn = fn;
        p->a0 = value;
        p->a1 = is_signal ? c->signal_arg : c->finish_arg;
    }
    ge_unlock();
}

void psp_ge_deliver(void) {
    /* A callback may wait, and every wait comes back through here. */
    if (g_ge_delivering || !g_ge_npending || psp_threadman_intr_disabled()) return;
    g_ge_delivering = 1;
    ge_pending batch[MAX_GE_PENDING];
    ge_lock();
    const int count = g_ge_npending;
    memcpy(batch, g_ge_pending, (size_t)count * sizeof batch[0]);
    g_ge_npending = 0;
    ge_unlock();
    for (int i = 0; i < count; i++) {
        const ge_pending p = batch[i];
        if (!psp_lookup(p.fn)) {
            fprintf(stderr, "psprecomp: GE callback 0x%08X is not a recompiled function\n", p.fn);
            continue;
        }
        /* It interrupts whatever was running, which must not notice. */
        psp_cpu_state saved = psp_cpu;
        psp_cpu.r[PSP_REG_A0] = p.a0;
        psp_cpu.r[PSP_REG_A1] = p.a1;
        psp_cpu.r[PSP_REG_RA] = 0;
        psp_call_from_host(p.fn);
        psp_cpu = saved;
        g_ge_delivered++;
    }
    g_ge_delivering = 0;
}
static uint32_t g_next_id;

static uint64_t g_unsupported;     /* vertices in a format we do not read */

/* Draw state handed to the backend with every primitive, and the raw words it
 * is decoded from where one value spans two commands. */
static psp_draw_state g_ds;
static uint32_t g_tbp, g_tbw, g_cbp, g_psm;
static uint8_t  g_clut[1024];      /* the GE's CLUT cache, filled by CLOAD */
/* A vertex with no colour takes the material colour: AMC's RGB, AMA's alpha. */
static uint32_t g_material = 0xFFFFFFFFu;

/* Lighting. Up to four lights, in world space like the normals they are
 * compared with. CMAT says which material colours the vertex colour stands in
 * for: bit 0 ambient, 1 diffuse, 2 specular. */
static struct {
    int      enable, on[4], type[4], comp[4], cmat, rnorm;
    uint32_t emissive, diffuse, specular;       /* EMC, DMC, SMC */
    uint32_t ambient;                           /* ALC | ALA << 24 */
    float    spow;
    float    pos[4][3], dir[4][3], att[4][3], spot_exp[4], spot_cut[4];
    uint32_t col[4][3];                         /* ambient, diffuse, specular */
} g_lt;
static uint32_t g_zbp, g_zbw;

#define GE_BONES 8

/* The transform pipeline. Matrices are uploaded one float per command, column
 * by column: world and view are 4x3 (the last column is the translation),
 * projection is 4x4. The viewport maps clip space into the GE's 4096-wide
 * drawing space, and the offset moves the screen's top-left within it. */
static struct {
    float world[12], view[12], proj[16];
    int   wi, vi, pi;
    float bone[GE_BONES * 12];          /* 4x3, uploaded like the world matrix */
    int   bi;
    float xscale, yscale, xpos, ypos;
    float offx, offy;                   /* pixels */
    float uscale, vscale, uoff, voff;
    float zscale, zpos;
    uint64_t behind;                    /* vertices behind the eye */
} g_xf;

/* GE floats are the top 24 bits of an IEEE single. */
static float f24(uint32_t arg) {
    union { uint32_t u; float f; } c;
    c.u = arg << 8;
    return c.f;
}

static void xf_reset(void) {
    memset(&g_xf, 0, sizeof g_xf);
    g_xf.world[0] = g_xf.world[4] = g_xf.world[8] = 1;
    g_xf.view[0] = g_xf.view[4] = g_xf.view[8] = 1;
    g_xf.proj[0] = g_xf.proj[5] = g_xf.proj[10] = g_xf.proj[15] = 1;
    for (int b = 0; b < GE_BONES; b++)
        g_xf.bone[b * 12 + 0] = g_xf.bone[b * 12 + 4] = g_xf.bone[b * 12 + 8] = 1;
    g_xf.uscale = g_xf.vscale = 1;
}

/* Model space to screen pixels. Returns 0 for a point behind the eye.
 * ponytail: no near-plane clipping, so a primitive with any vertex behind the
 * eye is dropped whole; add clipping when 3D scenes lose triangles. */
static int transform(const float p[3], float *sx, float *sy, float *sz, psp_vertex *out) {
    const float *W = g_xf.world, *V = g_xf.view, *P = g_xf.proj;
    float w[3], v[3], c[4];
    for (int r = 0; r < 3; r++)
        w[r] = W[r] * p[0] + W[3 + r] * p[1] + W[6 + r] * p[2] + W[9 + r];
    for (int r = 0; r < 3; r++)
        v[r] = V[r] * w[0] + V[3 + r] * w[1] + V[6 + r] * w[2] + V[9 + r];
    for (int r = 0; r < 4; r++)
        c[r] = P[r] * v[0] + P[4 + r] * v[1] + P[8 + r] * v[2] + P[12 + r];
    out->px = c[0] * g_xf.xscale + c[3] * (g_xf.xpos - g_xf.offx);
    out->py = c[1] * g_xf.yscale + c[3] * (g_xf.ypos - g_xf.offy);
    out->pz = c[2] * g_xf.zscale + c[3] * g_xf.zpos;
    out->pw = c[3];
    if (c[3] <= 0) return 0;
    *sx = c[0] / c[3] * g_xf.xscale + g_xf.xpos - g_xf.offx;
    *sy = c[1] / c[3] * g_xf.yscale + g_xf.ypos - g_xf.offy;
    *sz = c[2] / c[3] * g_xf.zscale + g_xf.zpos;
    return 1;
}

/* ---- lighting ------------------------------------------------------------ */

static void unpack(uint32_t c, float out[4]) {
    for (int i = 0; i < 4; i++) out[i] = (float)((c >> (8 * i)) & 0xFF);
}

static float norm3(float v[3]) {
    const float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 0) for (int i = 0; i < 3; i++) v[i] /= l;
    return l;
}

/* The lit colour of a vertex at model-space `p` with normal `n`, in world
 * space: emissive + ambient light x ambient material, plus for each enabled
 * light its ambient, its diffuse by N.L, and its specular by (N.H)^SPOW with
 * the viewer along +z, all scaled by distance attenuation and the spot cone.
 * Alpha is the ambient material's. */
static uint32_t light_vertex(const float p[3], const float n[3], uint32_t vcol) {
    const float *W = g_xf.world;
    float wp[3], wn[3];
    for (int r = 0; r < 3; r++) {
        wp[r] = W[r] * p[0] + W[3 + r] * p[1] + W[6 + r] * p[2] + W[9 + r];
        wn[r] = W[r] * n[0] + W[3 + r] * n[1] + W[6 + r] * n[2];
    }
    if (g_lt.rnorm) for (int r = 0; r < 3; r++) wn[r] = -wn[r];
    norm3(wn);

    float ma[4], md[4], ms[4], e[4], al[4], out[4];
    unpack(g_lt.cmat & 1 ? vcol : g_material, ma);
    unpack(g_lt.cmat & 2 ? vcol : g_lt.diffuse, md);
    unpack(g_lt.cmat & 4 ? vcol : g_lt.specular, ms);
    unpack(g_lt.emissive, e);
    unpack(g_lt.ambient, al);
    for (int k = 0; k < 3; k++) out[k] = e[k] + al[k] * ma[k] / 255.0f;

    for (int i = 0; i < 4; i++) {
        if (!g_lt.on[i]) continue;
        float L[3], att = 1.0f;
        if (g_lt.type[i] == 0) {                /* directional: pos is the direction */
            for (int r = 0; r < 3; r++) L[r] = g_lt.pos[i][r];
            norm3(L);
        } else {
            for (int r = 0; r < 3; r++) L[r] = g_lt.pos[i][r] - wp[r];
            const float d = norm3(L);
            const float den = g_lt.att[i][0] + g_lt.att[i][1] * d + g_lt.att[i][2] * d * d;
            att = den > 0 ? 1.0f / den : 1.0f;
            if (att > 1.0f) att = 1.0f;
            if (g_lt.type[i] == 2) {            /* spot */
                float D[3] = { g_lt.dir[i][0], g_lt.dir[i][1], g_lt.dir[i][2] };
                norm3(D);
                const float cs = -(L[0] * D[0] + L[1] * D[1] + L[2] * D[2]);
                att *= cs > g_lt.spot_cut[i] ? powf(cs, g_lt.spot_exp[i]) : 0.0f;
            }
        }
        float la[4], ld[4], ls[4];
        unpack(g_lt.col[i][0], la);
        unpack(g_lt.col[i][1], ld);
        unpack(g_lt.col[i][2], ls);
        float ndl = wn[0] * L[0] + wn[1] * L[1] + wn[2] * L[2];
        if (ndl < 0) ndl = 0;
        if (g_lt.comp[i] == 2) ndl = powf(ndl, g_lt.spow);
        float spec = 0;
        if (g_lt.comp[i] == 1 && ndl > 0) {
            float H[3] = { L[0], L[1], L[2] + 1.0f };
            norm3(H);
            const float ndh = wn[0] * H[0] + wn[1] * H[1] + wn[2] * H[2];
            if (ndh > 0) spec = powf(ndh, g_lt.spow);
        }
        for (int k = 0; k < 3; k++)
            out[k] += att * (la[k] * ma[k] + ld[k] * md[k] * ndl + ls[k] * ms[k] * spec) / 255.0f;
    }
    uint32_t c = (uint32_t)ma[3] << 24;
    for (int k = 0; k < 3; k++) {
        const int v = out[k] > 255.0f ? 255 : (int)out[k];
        c |= (uint32_t)v << (8 * k);
    }
    return c;
}

/* Light and material commands, which come in blocks of three or four. */
static int light_cmd(uint32_t cmd, uint32_t arg) {
    if (cmd >= 0x18 && cmd <= 0x1B) { g_lt.on[cmd - 0x18] = arg & 1; return 1; }
    if (cmd >= 0x5F && cmd <= 0x62) {
        g_lt.comp[cmd - 0x5F] = (int)(arg & 3);
        g_lt.type[cmd - 0x5F] = (int)((arg >> 8) & 3);
        return 1;
    }
    if (cmd >= 0x63 && cmd <= 0x6E) { g_lt.pos[(cmd - 0x63) / 3][(cmd - 0x63) % 3] = f24(arg); return 1; }
    if (cmd >= 0x6F && cmd <= 0x7A) { g_lt.dir[(cmd - 0x6F) / 3][(cmd - 0x6F) % 3] = f24(arg); return 1; }
    if (cmd >= 0x7B && cmd <= 0x86) { g_lt.att[(cmd - 0x7B) / 3][(cmd - 0x7B) % 3] = f24(arg); return 1; }
    if (cmd >= 0x87 && cmd <= 0x8A) { g_lt.spot_exp[cmd - 0x87] = f24(arg); return 1; }
    if (cmd >= 0x8B && cmd <= 0x8E) { g_lt.spot_cut[cmd - 0x8B] = f24(arg); return 1; }
    if (cmd >= 0x8F && cmd <= 0x9A) { g_lt.col[(cmd - 0x8F) / 3][(cmd - 0x8F) % 3] = arg; return 1; }
    switch (cmd) {
    case GE_LTE:   g_lt.enable = arg & 1; return 1;
    case GE_RNORM: g_lt.rnorm = arg & 1; return 1;
    case GE_CMAT:  g_lt.cmat = (int)(arg & 7); return 1;
    case GE_EMC:   g_lt.emissive = arg; return 1;
    case GE_DMC:   g_lt.diffuse = arg; return 1;
    case GE_SMC:   g_lt.specular = arg; return 1;
    case GE_SPOW:  g_lt.spow = f24(arg); return 1;
    case GE_ALC:   g_lt.ambient = (g_lt.ambient & 0xFF000000u) | arg; return 1;
    case GE_ALA:   g_lt.ambient = (g_lt.ambient & 0x00FFFFFFu) | ((arg & 0xFF) << 24); return 1;
    default:       return 0;
    }
}

static int g_cull_on, g_cull_face;

/* Tracked state, and the counters that make the report worth reading. */
static struct {
    uint32_t fbp, fbw, vtype, vaddr, iaddr;
    uint64_t commands;
    uint64_t prims[8];
    uint64_t vertices;
    uint64_t unknown;
    uint64_t lists;
    uint64_t finishes;
    uint64_t op_count[256];     /* undecoded commands, by opcode */
    uint32_t op_last[256];      /* and the last argument each one carried */
} g_ge;

void psp_ge_reset(void) {
    memset(g_queue, 0, sizeof g_queue);
    memset(g_ge_cb, 0, sizeof g_ge_cb);
    g_cull_on = g_cull_face = 0;
    g_ge_npending = 0;
    g_ge_delivering = 0;
    g_ge_delivered = g_ge_dropped = 0;
    memset(&g_ge, 0, sizeof g_ge);
    memset(&g_ds, 0, sizeof g_ds);
    memset(g_clut, 0, sizeof g_clut);
    g_tbp = g_tbw = g_cbp = 0;
    g_psm = 3;
    g_material = 0xFFFFFFFFu;
    g_ds.clut = g_clut;
    g_ds.clut_mask = 0xFF;
    g_ds.atest_func = 1;
    g_ds.atest_mask = 0xFF;
    g_ds.tex_w = g_ds.tex_h = 1;
    g_ds.tex_alpha = 1;
    g_ds.sc_x1 = 479;
    g_ds.sc_y1 = 271;
    g_ds.zwrite = 1;
    memset(&g_lt, 0, sizeof g_lt);
    g_zbp = g_zbw = 0;
    xf_reset();
    psp_render_reset_pixels();
    g_unsupported = 0;
    g_next_id = 0x00080000u;
}

void psp_ge_init(void) { psp_ge_reset(); }

void psp_ge_dump_stats(FILE *out) {
    fprintf(out, "GE: %llu lists, %llu commands, %llu finishes\n",
            (unsigned long long)g_ge.lists,
            (unsigned long long)g_ge.commands,
            (unsigned long long)g_ge.finishes);
    fprintf(out, "    framebuffer 0x%08X stride %u, vertex type 0x%06X\n",
            g_ge.fbp, g_ge.fbw, g_ge.vtype);
    fprintf(out, "    vertices submitted: %llu\n", (unsigned long long)g_ge.vertices);
    for (int i = 0; i < 8; i++)
        if (g_ge.prims[i])
            fprintf(out, "    %-15s %llu\n", PRIM_NAME[i], (unsigned long long)g_ge.prims[i]);
    if (g_ge.unknown) {
        fprintf(out, "    %llu commands not individually decoded:\n",
                (unsigned long long)g_ge.unknown);
        for (int i = 0; i < 256; i++)
            if (g_ge.op_count[i])
                fprintf(out, "      0x%02X x%-8llu last arg 0x%06X\n", i,
                        (unsigned long long)g_ge.op_count[i], g_ge.op_last[i]);
    }
    fprintf(out, "    pixels written: %llu\n", (unsigned long long)psp_render_pixels());
    if (g_xf.behind)
        fprintf(out, "    %llu vertices behind the eye; their triangles dropped (no clipping)\n",
                (unsigned long long)g_xf.behind);
    if (psp_render_unsupported_texels())
        fprintf(out, "    %llu texels in an undecoded format (DXT), drawn magenta\n",
                (unsigned long long)psp_render_unsupported_texels());
    if (g_unsupported)
        fprintf(out, "    %llu vertices in an unsupported format (skinned, or no position)\n",
                (unsigned long long)g_unsupported);
}

uint64_t psp_ge_command_count(void) { return g_ge.commands; }
uint64_t psp_ge_vertex_count(void)  { return g_ge.vertices; }

/* ---- rasterizer ----------------------------------------------------------
 *
 * Enough of the GE to put pixels in the framebuffer. Deliberately narrow:
 *
 *  - "through" mode (VTYPE bit 23), where vertex coordinates are already in
 *    screen space and texture coordinates are texels, and transformed mode,
 *    where positions go through world, view and projection and the viewport,
 *    and texture coordinates are normalised and scaled by UV scale/offset.
 *    Transformed vertices are skinned, lit (four lights, per vertex) and get a
 *    depth. Morphing, the texture matrix and near-plane clipping are not done.
 *  - Position, normal and texture coordinates as 8- or 16-bit or float;
 *    colour as 8888, the 16-bit formats, or none; 8- or 16-bit indices.
 *  - Textures, CLUTs, the texture function, alpha test, depth test, blending,
 *    clear mode and scissor are decoded into psp_draw_state for the backend.
 *
 * The point is to close the loop from display list to visible pixels so the
 * rest can be measured against something. Everything omitted is omitted
 * loudly: unsupported vertex formats are counted, not guessed at.
 */

/* VTYPE field extraction. */
#define VT_TEX(v)     ((v) & 3)
#define VT_COLOR(v)   (((v) >> 2) & 7)
#define VT_NORMAL(v)  (((v) >> 5) & 3)
#define VT_POS(v)     (((v) >> 7) & 3)
#define VT_WEIGHT(v)  (((v) >> 9) & 3)
#define VT_INDEX(v)   (((v) >> 11) & 3)
#define VT_WEIGHTS(v) ((int)(((v) >> 14) & 7) + 1)
#define VT_MORPHS(v)  ((int)(((v) >> 18) & 7) + 1)
#define VT_THROUGH(v) (((v) >> 23) & 1)


uint64_t psp_ge_pixels(void) { return psp_render_pixels(); }

/* Size of one vertex in bytes, and the offsets within it. Components appear in
 * a fixed order (weights, texture, colour, normal, position) and each is
 * aligned to its own size, which is what makes the stride awkward enough to be
 * worth computing rather than assuming. */
static int vertex_layout(uint32_t vtype, int *tex_off, int *col_off, int *nrm_off, int *pos_off) {
    static const int tex_sz[4]   = { 0, 1, 2, 4 };
    static const int col_sz[8]   = { 0, 0, 0, 0, 2, 2, 2, 4 };
    static const int norm_sz[4]  = { 0, 1, 2, 4 };
    static const int pos_sz[4]   = { 0, 1, 2, 4 };

    int off = 0, align = 1;
    int t = tex_sz[VT_TEX(vtype)] * 2;
    int c = col_sz[VT_COLOR(vtype)];
    int n = norm_sz[VT_NORMAL(vtype)] * 3;
    int p = pos_sz[VT_POS(vtype)] * 3;

    if (VT_MORPHS(vtype) > 1) return 0;      /* morphing: not handled */

    /* Weights come first, so they always start at offset 0. */
    int ws = pos_sz[VT_WEIGHT(vtype)];
    if (ws) { off = ws * VT_WEIGHTS(vtype); align = ws; }

    int ts = tex_sz[VT_TEX(vtype)];
    if (ts) { off = (off + ts - 1) & ~(ts - 1); *tex_off = off; off += t; if (ts > align) align = ts; }
    else *tex_off = -1;
    int cs = col_sz[VT_COLOR(vtype)];
    if (cs) { off = (off + cs - 1) & ~(cs - 1); *col_off = off; off += c; if (cs > align) align = cs; }
    else *col_off = -1;
    int ns = norm_sz[VT_NORMAL(vtype)];
    if (ns) { off = (off + ns - 1) & ~(ns - 1); *nrm_off = off; off += n; if (ns > align) align = ns; }
    else *nrm_off = -1;
    int ps = pos_sz[VT_POS(vtype)];
    if (!ps) return 0;                        /* no position: nothing to draw */
    off = (off + ps - 1) & ~(ps - 1); *pos_off = off; off += p;
    if (ps > align) align = ps;

    return (off + align - 1) & ~(align - 1);  /* stride */
}

/* Three components of 8 or 16 bits or float, as fixed point unless `raw`. */
static void read3(uint32_t a, int fmt, int raw, float v[3]) {
    for (int i = 0; i < 3; i++) {
        switch (fmt) {
        case 1:  v[i] = (float)(int8_t)psp_fast_read8(a + (uint32_t)i);
                 if (!raw) v[i] /= 128.0f; break;
        case 2:  v[i] = (float)(int16_t)psp_fast_read16(a + (uint32_t)i * 2);
                 if (!raw) v[i] /= 32768.0f; break;
        default: v[i] = psp_fast_read_f32(a + (uint32_t)i * 4); break;
        }
    }
}

/* Replace a position and normal with their weighted sum over the bone matrices.
 * Weights are unsigned fixed point where 1.0 is 0x80 or 0x8000, and a normal takes
 * no translation. Each vertex's weights sit at its very start. */
static void skin_vertex(uint32_t addr, uint32_t vtype, float pos[3], float n[3]) {
    const float p0[3] = { pos[0], pos[1], pos[2] }, n0[3] = { n[0], n[1], n[2] };
    const int fmt = (int)VT_WEIGHT(vtype), count = VT_WEIGHTS(vtype);
    for (int r = 0; r < 3; r++) pos[r] = n[r] = 0;
    for (int b = 0; b < count; b++) {
        float w;
        if (fmt == 1)      w = (float)psp_fast_read8(addr + (uint32_t)b) / 128.0f;
        else if (fmt == 2) w = (float)psp_fast_read16(addr + (uint32_t)b * 2) / 32768.0f;
        else               w = psp_fast_read_f32(addr + (uint32_t)b * 4);
        const float *B = &g_xf.bone[b * 12];
        for (int r = 0; r < 3; r++) {
            pos[r] += w * (B[r] * p0[0] + B[3 + r] * p0[1] + B[6 + r] * p0[2] + B[9 + r]);
            n[r]   += w * (B[r] * n0[0] + B[3 + r] * n0[1] + B[6 + r] * n0[2]);
        }
    }
}

static int read_vertex(uint32_t addr, uint32_t vtype, int tex_off, int col_off,
                       int nrm_off, int pos_off, psp_vertex *out) {
    float pos[3];
    /* Through mode reads 8/16-bit values as integers; transformed mode reads
     * them as fixed point, signed for positions. */
    const int through = (int)VT_THROUGH(vtype);
    out->rgba = g_material;
    if (col_off >= 0) {
        if (VT_COLOR(vtype) == 7)
            out->rgba = psp_fast_read32(addr + (uint32_t)col_off);
        else    /* 4, 5, 6: 565, 5551, 4444 -- the texture formats, numbered from 4 */
            out->rgba = psp_color_expand(psp_fast_read16(addr + (uint32_t)col_off),
                                         (int)VT_COLOR(vtype) - 4);
    }

    out->u = out->v = 0;
    if (tex_off >= 0) {
        uint32_t t = addr + (uint32_t)tex_off;
        switch (VT_TEX(vtype)) {
        case 1: out->u = psp_fast_read8(t);  out->v = psp_fast_read8(t + 1);
                if (!through) { out->u /= 128.0f; out->v /= 128.0f; }
                break;
        case 2: out->u = psp_fast_read16(t); out->v = psp_fast_read16(t + 2);
                if (!through) { out->u /= 32768.0f; out->v /= 32768.0f; }
                break;
        default: out->u = psp_fast_read_f32(t); out->v = psp_fast_read_f32(t + 4); break;
        }
        if (!through) {     /* normalised, scaled, then in texels */
            out->u = (out->u * g_xf.uscale + g_xf.uoff) * (float)g_ds.tex_w;
            out->v = (out->v * g_xf.vscale + g_xf.voff) * (float)g_ds.tex_h;
        }
    }

    if (!VT_POS(vtype)) return 0;
    read3(addr + (uint32_t)pos_off, (int)VT_POS(vtype), through, pos);
    out->behind = 0;
    if (through) {
        /* Through mode's depth is an unsigned 16-bit value. */
        out->x = (int)pos[0];
        out->y = (int)pos[1];
        out->z = VT_POS(vtype) == 2 ? (float)psp_fast_read16(addr + (uint32_t)pos_off + 4) : pos[2];
        out->px = pos[0];
        out->py = pos[1];
        out->pz = out->z;
        out->pw = 1.0f;
        return 1;
    }
    float n[3] = { 0, 0, 1 };
    if (nrm_off >= 0) read3(addr + (uint32_t)nrm_off, (int)VT_NORMAL(vtype), 0, n);
    if (VT_WEIGHT(vtype)) skin_vertex(addr, vtype, pos, n);
    if (g_lt.enable) out->rgba = light_vertex(pos, n, out->rgba);
    float sx, sy, sz;
    if (!transform(pos, &sx, &sy, &sz, out)) { out->behind = 1; g_xf.behind++; return 1; }
    out->x = (int)floorf(sx);
    out->y = (int)floorf(sy);
    out->z = sz;
    return 1;
}


/* PSPRECOMP_TRACE_GE=<frame> logs every primitive drawn during that vblank:
 * what it is, its vertex format, and the texture state it is drawn with. */
static int trace_frame(void) {
    static long f = -2;
    if (f == -2) { const char *e = getenv("PSPRECOMP_TRACE_GE"); f = e ? atol(e) : -1; }
    return f >= 0 && (long)psp_display_vblanks() == f;
}

/* GE_CULL 0 makes counter-clockwise triangles the front, so clockwise ones go, and
 * 1 the reverse. Through-mode drawing is flat screen art and is never culled. */
static void draw_prim(uint32_t type, uint32_t count) {
    g_ds.cull = (g_cull_on && !VT_THROUGH(g_ge.vtype) && !g_ds.clear)
                    ? (g_cull_face ? PSP_CULL_CCW : PSP_CULL_CW) : PSP_CULL_NONE;
    if (trace_frame())
        fprintf(stderr, "GE prim %u x%u vtype %06X vaddr %08X tex %d fmt %d %dx%d bw %d at %08X sw %d func %d clut fmt %d blend %d atest %d clear %d z %d/%d/%d\n",
                type, count, g_ge.vtype, g_ge.vaddr, g_ds.tex_enable, g_ds.tex_fmt, g_ds.tex_w, g_ds.tex_h,
                g_ds.tex_bw, g_ds.tex_addr, g_ds.tex_swizzle, g_ds.tex_func, g_ds.clut_fmt, g_ds.blend,
                g_ds.atest, g_ds.clear, g_ds.ztest, g_ds.zfunc, g_ds.zwrite);
    if (!g_ge.vaddr) { g_unsupported += count; return; }

    int tex_off = -1, col_off = -1, nrm_off = -1, pos_off = 0;
    int stride = vertex_layout(g_ge.vtype, &tex_off, &col_off, &nrm_off, &pos_off);
    if (!stride) { g_unsupported += count; return; }

    /* Indexed: the vertices are vaddr[index[i]], with 8- or 16-bit indices.
     * Format decoding stays here rather than in each backend: the stride
     * arithmetic and component alignment are fiddly, and duplicating them per
     * backend means every backend is wrong in its own way. */
    static const int ISZ[4] = { 0, 1, 2, 4 };
    const int isz = ISZ[VT_INDEX(g_ge.vtype)];
    if (isz && !g_ge.iaddr) { g_unsupported += count; return; }

    static psp_vertex v[65536];
    uint32_t n = 0;
    for (; n < count; n++) {
        uint32_t idx = n;
        if (isz == 1)      idx = psp_fast_read8(g_ge.iaddr + n);
        else if (isz == 2) idx = psp_fast_read16(g_ge.iaddr + n * 2);
        else if (isz == 4) idx = psp_fast_read32(g_ge.iaddr + n * 4);
        if (!read_vertex(g_ge.vaddr + idx * (uint32_t)stride, g_ge.vtype,
                         tex_off, col_off, nrm_off, pos_off, &v[n])) break;
    }
    /* The GE leaves its read pointer after what a primitive consumed, so the
     * next PRIM continues from there without a new VADDR (or IADDR). */
    if (isz) g_ge.iaddr += count * (uint32_t)isz;
    else     g_ge.vaddr += count * (uint32_t)stride;

    if (trace_frame()) {
        fprintf(stderr, "  mat %08X cbp %08X lt %d tex0 %08X\n", g_material, g_cbp, g_lt.enable,
                psp_fast_read32(g_ds.tex_addr));
        for (uint32_t k = 0; k < n && k < 16; k++)
            fprintf(stderr, "  v%u (%d,%d,%.0f) rgba %08X uv %.1f %.1f texel %08X\n", k, v[k].x, v[k].y,
                    (double)v[k].z, v[k].rgba, v[k].u, v[k].v, psp_tex_fetch(&g_ds, (int)v[k].u, (int)v[k].v));
    }
    if (n) psp_render_current()->draw((int)type, v, (int)n, &g_ds);
}

/* The framebuffer is always in eDRAM. Games send FBP relative to it, so a high
 * byte of 0 is the norm, and an absolute 0x04xxxxxx folds to the same place.
 * Taken literally, 0x88000 is main RAM: for a vaddr-0 module, its own code. */
static uint32_t fb_vram(void) { return PSP_VRAM_BASE | (g_ge.fbp & (PSP_VRAM_SIZE - 1)); }

/* Walk a list until END/FINISH, the stall address, or a step budget.
 *
 * The budget is not paranoia: a list whose JUMP forms a cycle is a normal
 * intermediate state while the CPU is still writing, and without a bound a
 * malformed or partially-written list hangs the host with no diagnostic. */
static void run_list(ge_queue *q) {
    uint32_t stack[GE_STACK];
    int sp = 0;
    uint64_t budget = 1u << 22;

    g_ge.lists++;

    while (budget--) {
        if (q->stall && q->list == q->stall) break;   /* caught up to the CPU */

        uint32_t word = psp_fast_read32(q->list);
        uint32_t cmd  = word >> 24;
        uint32_t arg  = word & 0x00FFFFFF;
        q->list += 4;
        g_ge.commands++;
        if (trace_frame()) fprintf(stderr, "GE @%08X %02X %06X\n", q->list - 4, cmd, arg);

        switch (cmd) {
        case GE_NOP:
            break;

        case GE_PRIM: {
            uint32_t type  = (arg >> 16) & 7;
            uint32_t count = arg & 0xFFFF;
            g_ge.prims[type]++;
            g_ge.vertices += count;
            draw_prim(type, count);
            break;
        }
        case GE_BEZIER:
        case GE_SPLINE:
            /* Patches expand to triangles on hardware; counted as their own
             * thing rather than folded into the triangle count. */
            g_ge.prims[3]++;
            break;

        case GE_JUMP:
            q->list = (q->base | (arg & 0xFFFFFC));
            break;
        case GE_CALL:
            if (sp < GE_STACK) stack[sp++] = q->list;
            q->list = (q->base | (arg & 0xFFFFFC));
            break;
        case GE_RET:
            if (sp > 0) q->list = stack[--sp];
            break;
        case GE_BJUMP:
            /* Conditional on the bounding-box test, which needs geometry we do
             * not process. Not taking it means we walk the enclosed commands
             * rather than skipping them -- the conservative direction, since
             * skipping would under-report what the game drew. */
            break;

        case GE_END:
        case GE_FINISH:
            if (cmd == GE_FINISH) {
                g_ge.finishes++;
                ge_raise(q, 0, arg & 0xFFFF);
            }
            /* A finished list gives its slot back. Keeping it used meant the
             * ninth enqueue failed, and WTF then dropped 3,870 of 3,885
             * frames while running at full speed. */
            ge_lock();
            q->done = 1;
            q->used = 0;
            ge_unlock();
            return;

        case GE_SIGNAL:
            /* SIGNAL always comes paired with an END that carries more of its
             * arguments. That END does not end the list, so it is consumed here.
             * The behaviours that move the read pointer are not handled. */
            ge_raise(q, 1, arg & 0xFFFF);
            if ((psp_fast_read32(q->list) >> 24) == GE_END) q->list += 4;
            break;

        case GE_BASE:        q->base = (arg & 0xFF0000) << 8; break;
        case GE_ORIGIN_ADDR: q->origin = q->list - 4; break;
        case GE_OFFSET_ADDR: q->base = arg << 8; break;

        case GE_VTYPE: g_ge.vtype = arg; break;
        case GE_FBP:
            g_ge.fbp = (g_ge.fbp & 0xFF000000u) | arg;
            psp_render_current()->set_target(fb_vram(), g_ge.fbw, (int)g_psm);
            break;
        case GE_FBW:
            g_ge.fbw = arg & 0xFFFF;
            g_ge.fbp = (g_ge.fbp & 0x00FFFFFFu) | ((arg & 0xFF0000) << 8);
            psp_render_current()->set_target(fb_vram(), g_ge.fbw, (int)g_psm);
            break;

        case GE_PSM:
            g_psm = arg & 3;
            psp_render_current()->set_target(fb_vram(), g_ge.fbw, (int)g_psm);
            break;

        case GE_TFLT:
            g_ds.tex_min_linear = (int)(arg & 1);
            g_ds.tex_mag_linear = (int)((arg >> 8) & 1);
            break;
        case GE_BCE:   g_cull_on = arg & 1; break;
        case GE_CULL:  g_cull_face = arg & 1; break;
        case GE_BOFS:  g_xf.bi = (int)(arg & 0x7F); break;
        case GE_BONE:  if (g_xf.bi < GE_BONES * 12) g_xf.bone[g_xf.bi++] = f24(arg); break;
        case GE_WMS:   g_xf.wi = (int)(arg & 0xF); break;
        case GE_WORLD: if (g_xf.wi < 12) g_xf.world[g_xf.wi++] = f24(arg); break;
        case GE_VMS:   g_xf.vi = (int)(arg & 0xF); break;
        case GE_VIEW:  if (g_xf.vi < 12) g_xf.view[g_xf.vi++] = f24(arg); break;
        case GE_PMS:   g_xf.pi = (int)(arg & 0xF); break;
        case GE_PROJ:  if (g_xf.pi < 16) g_xf.proj[g_xf.pi++] = f24(arg); break;
        case GE_XSCALE:  g_xf.xscale = f24(arg); break;
        case GE_YSCALE:  g_xf.yscale = f24(arg); break;
        case GE_XPOS:    g_xf.xpos = f24(arg); break;
        case GE_YPOS:    g_xf.ypos = f24(arg); break;
        case GE_USCALE:  g_xf.uscale = f24(arg); break;
        case GE_VSCALE:  g_xf.vscale = f24(arg); break;
        case GE_UOFFSET: g_xf.uoff = f24(arg); break;
        case GE_VOFFSET: g_xf.voff = f24(arg); break;
        case GE_OFFSETX: g_xf.offx = (float)(arg & 0xFFFF) / 16.0f; break;   /* 12.4 */
        case GE_OFFSETY: g_xf.offy = (float)(arg & 0xFFFF) / 16.0f; break;

        case GE_TME:   g_ds.tex_enable = arg & 1; break;
        case GE_ABE:   g_ds.blend = arg & 1; break;
        case GE_ATE:   g_ds.atest = arg & 1; break;
        case GE_SHADE: g_ds.gouraud = arg & 1; break;
        case GE_AMC:   g_material = (g_material & 0xFF000000u) | arg; break;
        case GE_AMA:   g_material = (g_material & 0x00FFFFFFu) | ((arg & 0xFF) << 24); break;
        case GE_CLEAR:
            g_ds.clear = arg & 1;
            g_ds.clear_color = (arg >> 8) & 1;
            g_ds.clear_alpha = (arg >> 9) & 1;
            g_ds.clear_depth = (arg >> 10) & 1;
            break;

        /* Depth. The buffer is 16 bits per pixel in VRAM; ZBW carries the
         * address's high byte like FBW does. */
        case GE_ZTE:    g_ds.ztest = arg & 1; break;
        case GE_ZTST:   g_ds.zfunc = (int)(arg & 7); break;
        case GE_ZMSK:   g_ds.zwrite = !(arg & 1); break;
        case GE_ZSCALE: g_xf.zscale = f24(arg); break;
        case GE_ZPOS:   g_xf.zpos = f24(arg); break;
        case GE_ZBP:
        case GE_ZBW:
            if (cmd == GE_ZBP) g_zbp = arg; else g_zbw = arg;
            g_ds.zaddr = PSP_VRAM_BASE | ((((g_zbw & 0xFF0000u) << 8) | g_zbp) & (PSP_VRAM_SIZE - 1));
            g_ds.zbw = (int)(g_zbw & 0xFFFF);
            break;

        /* TBP0 carries the low 24 bits and TBW0 bits 24-31 in its top byte. */
        case GE_TBP0:
            g_tbp = arg;
            g_ds.tex_addr = ((g_tbw & 0xFF0000u) << 8) | g_tbp;
            break;
        case GE_TBW0:
            g_tbw = arg;
            g_ds.tex_bw = (int)(arg & 0xFFFF);
            g_ds.tex_addr = ((g_tbw & 0xFF0000u) << 8) | g_tbp;
            break;
        case GE_TSIZE0:
            g_ds.tex_w = 1 << (arg & 0xF);
            g_ds.tex_h = 1 << ((arg >> 8) & 0xF);
            break;
        case GE_TMODE: g_ds.tex_swizzle = arg & 1; break;
        case GE_TPSM:  g_ds.tex_fmt = (int)(arg & 0xF); break;
        case GE_TWRAP:
            g_ds.tex_clamp_u = arg & 1;
            g_ds.tex_clamp_v = (arg >> 8) & 1;
            break;
        case GE_TFUNC:
            g_ds.tex_func = (int)(arg & 7);
            g_ds.tex_alpha = (arg >> 8) & 1;
            g_ds.tex_double = (arg >> 16) & 1;
            break;
        case GE_TEC:   g_ds.tex_env = arg; break;

        /* The CLUT is copied into the GE when CLOAD runs, so a game may
         * rewrite the memory afterwards. CBPH carries bits 24-27. */
        case GE_CBP:   g_cbp = (g_cbp & 0xFF000000u) | arg; break;
        case GE_CBPH:  g_cbp = (g_cbp & 0x00FFFFFFu) | ((arg & 0x0F0000u) << 8); break;
        case GE_CLOAD: {
            uint32_t n = (arg & 0x3F) * 32u;
            if (n > sizeof g_clut) n = sizeof g_clut;
            for (uint32_t i = 0; i < n; i++) g_clut[i] = psp_fast_read8(g_cbp + i);
            break;
        }
        case GE_CMODE:
            g_ds.clut_fmt = (int)(arg & 3);
            g_ds.clut_shift = (int)((arg >> 2) & 0x1F);
            g_ds.clut_mask = (int)((arg >> 8) & 0xFF);
            g_ds.clut_start = (int)((arg >> 16) & 0x1F);
            break;

        case GE_SCISSOR1:
            g_ds.sc_x0 = (int)(arg & 0x3FF);
            g_ds.sc_y0 = (int)((arg >> 10) & 0x3FF);
            break;
        case GE_SCISSOR2:
            g_ds.sc_x1 = (int)(arg & 0x3FF);
            g_ds.sc_y1 = (int)((arg >> 10) & 0x3FF);
            break;

        case GE_ATST:
            g_ds.atest_func = (int)(arg & 7);
            g_ds.atest_ref = (int)((arg >> 8) & 0xFF);
            g_ds.atest_mask = (int)((arg >> 16) & 0xFF);
            break;
        case GE_ALPHA:
            g_ds.blend_src = (int)(arg & 0xF);
            g_ds.blend_dst = (int)((arg >> 4) & 0xF);
            g_ds.blend_op = (int)((arg >> 8) & 0xF);
            break;
        case GE_SFIX:  g_ds.blend_sfix = arg; break;
        case GE_DFIX:  g_ds.blend_dfix = arg; break;

        case GE_VADDR: g_ge.vaddr = (q->base | (arg & 0xFFFFFF)); break;
        case GE_IADDR: g_ge.iaddr = (q->base | (arg & 0xFFFFFF)); break;

        default:
            if (light_cmd(cmd, arg)) break;
            /* A real state command we do not decode individually. Counted --
             * and, for the first few, named. "240 commands not individually
             * decoded" hides whether the game is configuring a draw or just
             * poking state, which is the difference between a rendering bug
             * and a game that has not asked to render yet. */
            g_ge.unknown++;
            g_ge.op_count[cmd]++;
            g_ge.op_last[cmd] = arg;
            break;
        }
    }
}

/* ---- the calls ----------------------------------------------------------- */

static ge_queue *find_queue(uint32_t id) {
    for (int i = 0; i < MAX_QUEUES; i++)
        if (g_queue[i].used && g_queue[i].id == id) return &g_queue[i];
    return NULL;
}

static uint64_t g_ge_busy_us;

uint64_t psp_ge_busy_us(void) { return g_ge_busy_us; }

static uint64_t ge_now_us(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void run_list_timed(ge_queue *q) {
    const uint64_t start = ge_now_us();
    run_list(q);
    g_ge_busy_us += ge_now_us() - start;
}

/* ---- the GE's own thread ---------------------------------------------------
 *
 * Only one guest thread runs at a time, so a list drawn inside the enqueue stops
 * the whole game while it draws. Hardware draws while the CPU carries on, and a
 * game is written for that: it syncs before touching what a list uses. Doing the
 * same puts software rendering on a core of its own. */
static int g_ge_async, g_ge_thread_running, g_ge_inflight;

/* A buffer switch takes effect after the lists queued before it, or the screen
 * would show a frame the GE has not finished. */
static int g_flip_pending;
static uint32_t g_flip_after, g_flip_addr, g_flip_stride;
static int g_flip_fmt;

static void ge_worker(void) {
    if (psp_render_current()->thread_init && psp_render_current()->thread_init() != 0) {
        fprintf(stderr, "ge: falling back to the software renderer\n");
        psp_render_select("software");
        psp_window_set_external(0);
    }
    ge_lock();
    for (;;) {
        ge_queue *q = NULL;
        for (int i = 0; i < MAX_QUEUES; i++)
            if (g_queue[i].queued && (!q || g_queue[i].id < q->id)) q = &g_queue[i];
        if (g_flip_pending && (!q || q->id >= g_flip_after)) {
            const uint32_t addr = g_flip_addr, stride = g_flip_stride;
            const int fmt = g_flip_fmt;
            g_flip_pending = 0;
            ge_unlock();
            psp_render_current()->flip(addr, stride, fmt);
            ge_lock();
            continue;
        }
        if (!q) { ge_wait(); continue; }
        q->queued = 0;
        ge_unlock();
        run_list_timed(q);
        ge_lock();
        g_ge_inflight--;
        ge_wake();
        const int raised = g_ge_npending > 0;
        ge_unlock();
        /* Nothing else tells a sleeping game that its list is done. */
        if (raised) psp_threadman_kick();
        ge_lock();
    }
}

void psp_ge_set_async(int on) {
    if (on && !g_ge_thread_running) g_ge_thread_running = ge_thread_start(ge_worker);
    g_ge_async = on && g_ge_thread_running;
}

void psp_ge_flip(uint32_t addr, uint32_t stride, int fmt) {
    if (!g_ge_async || !psp_render_current()->flip) return;
    ge_lock();
    g_flip_pending = 1;
    g_flip_after = g_next_id;
    g_flip_addr = addr;
    g_flip_stride = stride;
    g_flip_fmt = fmt;
    ge_wake();
    ge_unlock();
}

/* A short list is done long before a sleeping thread could be woken to hear about
 * it, and a game can wait on dozens of them a frame. So a thread about to sleep on
 * the GE's account watches it for a moment first. Returns nonzero if a callback was
 * delivered, in which case what it was waiting for may have happened. */
int psp_ge_wait_briefly(uint32_t us) {
    if (!g_ge_async || psp_threadman_intr_disabled()) return 0;
    for (uint64_t end = ge_now_us() + us;;) {
        if (g_ge_npending) {
            const uint64_t before = g_ge_delivered;
            psp_ge_deliver();
            return g_ge_delivered != before;
        }
        if (!g_ge_inflight || ge_now_us() >= end) return 0;
    }
}

static int ge_busy(void) {
    ge_lock();
    const int busy = g_ge_inflight > 0;
    ge_unlock();
    return busy;
}

static void enqueue(int head) {
    /* (list, stall, cbid, arg) */
    const uint32_t list = psp_arg(0) & ~3u, stall = psp_arg(1) & ~3u, cbid = psp_arg(2);
    ge_queue *q = NULL;
    (void)head;

    for (;;) {
        ge_lock();
        for (int i = 0; i < MAX_QUEUES; i++) if (!g_queue[i].used) { q = &g_queue[i]; break; }
        if (q || !g_ge_async) break;
        ge_unlock();
        psp_threadman_yield();        /* the GE is behind: wait for a free slot */
    }
    if (!q) { ge_unlock(); psp_ret(SCE_KERNEL_ERROR_NO_MEMORY); return; }

    memset(q, 0, sizeof *q);
    const uint32_t id = g_next_id++;
    q->id    = id;
    q->list  = list;
    q->stall = stall;
    q->used  = 1;
    q->cbid  = cbid;

    if (g_ge_async) {
        q->queued = 1;
        g_ge_inflight++;
        ge_wake();
        ge_unlock();
    } else {
        ge_unlock();
        run_list_timed(q);
    }
    psp_ret(id);
}

static void hle_ListEnQueue(void)     { enqueue(0); }
static void hle_ListEnQueueHead(void) { enqueue(1); }

static void hle_ListUpdateStallAddr(void) {
    uint32_t id = psp_arg(0);
    ge_queue *q = find_queue(id);
    if (!q) {
        /* A list that already finished has released its slot; moving its
         * stall is harmless. An id never issued is an error. */
        psp_ret(id >= 0x00080000u && id < g_next_id ? SCE_KERNEL_ERROR_OK
                                                    : SCE_KERNEL_ERROR_UNKNOWN_UID);
        return;
    }
    if (g_ge_async) {
        ge_lock();
        q->stall = psp_arg(1) & ~3u;
        if (!q->done && !q->queued) {
            q->queued = 1;
            g_ge_inflight++;
            ge_wake();
        }
        ge_unlock();
    } else {
        q->stall = psp_arg(1) & ~3u;
        if (!q->done) run_list_timed(q);    /* the new stall released more commands */
    }
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* Mode 0 waits for the GE to finish, mode 1 only asks. */
#define GE_LIST_DRAWING 2u

static void ge_sync(uint32_t mode) {
    if (mode == 1 && ge_busy()) { psp_ret(GE_LIST_DRAWING); return; }
    while (ge_busy()) psp_threadman_yield();
    psp_ge_deliver();
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* (id, mode): every list runs in order, so this waits for them all. */
static void hle_ListSync(void) { ge_sync(psp_arg(1)); }
static void hle_DrawSync(void) { ge_sync(psp_arg(0)); }

static void hle_Break(void)    { psp_ret(SCE_KERNEL_ERROR_OK); }
static void hle_Continue(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

/* (PspGeCallbackData *): signal_func, signal_arg, finish_func, finish_arg */
static void hle_SetCallback(void) {
    const uint32_t data = psp_arg(0);
    for (uint32_t i = 0; i < MAX_GE_CALLBACKS; i++) {
        ge_callback *c = &g_ge_cb[i];
        if (c->used) continue;
        c->used = 1;
        c->signal_fn  = psp_fast_read32(data);
        c->signal_arg = psp_fast_read32(data + 4);
        c->finish_fn  = psp_fast_read32(data + 8);
        c->finish_arg = psp_fast_read32(data + 12);
        psp_ret(i);
        return;
    }
    psp_ret(SCE_KERNEL_ERROR_NO_MEMORY);
}

static void hle_UnsetCallback(void) {
    const uint32_t id = psp_arg(0);
    if (id < MAX_GE_CALLBACKS) g_ge_cb[id].used = 0;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* eDRAM is the GPU-visible VRAM window: 2 MB at 0x04000000. */
static void hle_EdramGetAddr(void) { psp_ret(PSP_VRAM_BASE); }
static void hle_EdramGetSize(void) { psp_ret(PSP_VRAM_SIZE); }

void psp_ge_register(void) {
    psp_hle_register(0xAB49E76A, "sceGe_user", "sceGeListEnQueue",         hle_ListEnQueue);
    psp_hle_register(0x1C0D95A6, "sceGe_user", "sceGeListEnQueueHead",     hle_ListEnQueueHead);
    psp_hle_register(0xE0D68148, "sceGe_user", "sceGeListUpdateStallAddr", hle_ListUpdateStallAddr);
    psp_hle_register(0x03444EB4, "sceGe_user", "sceGeListSync",            hle_ListSync);
    psp_hle_register(0xB287BD61, "sceGe_user", "sceGeDrawSync",            hle_DrawSync);
    psp_hle_register(0xB448EC0D, "sceGe_user", "sceGeBreak",               hle_Break);
    psp_hle_register(0x4C06E472, "sceGe_user", "sceGeContinue",            hle_Continue);
    psp_hle_register(0xA4FC06A4, "sceGe_user", "sceGeSetCallback",         hle_SetCallback);
    psp_hle_register(0x05DB22CE, "sceGe_user", "sceGeUnsetCallback",       hle_UnsetCallback);
    psp_hle_register(0xE47E40E4, "sceGe_user", "sceGeEdramGetAddr",        hle_EdramGetAddr);
    psp_hle_register(0x1F6752AD, "sceGe_user", "sceGeEdramGetSize",        hle_EdramGetSize);
}
