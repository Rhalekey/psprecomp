/* psprecomp — sceDisplay.
 *
 * The PSP's display is a framebuffer address plus a pixel format, scanned out
 * at ~59.94 Hz. There is no blitting and no compositing: a game writes pixels
 * (usually via the GE) and tells sceDisplay where they are.
 *
 * That makes this small but pivotal — it is the first point in bring-up where
 * a recompiled game produces something you can *look at*. psp_display_capture()
 * exists for exactly that: dump whatever the game currently considers the
 * front buffer, and compare it against the same frame from an emulator.
 */

#include "psprecomp/hle.h"
#include "psprecomp/render.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define PSP_SCREEN_W 480
#define PSP_SCREEN_H 272

/* Pixel formats, as passed to sceDisplaySetFrameBuf. */
#define PSP_DISPLAY_PIXEL_FORMAT_565  0
#define PSP_DISPLAY_PIXEL_FORMAT_5551 1
#define PSP_DISPLAY_PIXEL_FORMAT_4444 2
#define PSP_DISPLAY_PIXEL_FORMAT_8888 3

static uint32_t g_fb_addr;
static uint32_t g_fb_width;      /* in pixels, the stride -- usually 512 */
static uint32_t g_fb_format;
static uint32_t g_mode, g_mode_w, g_mode_h;
static uint64_t g_vblank_count;
static uint64_t g_flips;     /* frames the game actually presented */

void psp_display_reset(void) {
    g_fb_addr = 0;
    g_fb_width = 512;
    g_fb_format = PSP_DISPLAY_PIXEL_FORMAT_8888;
    g_mode = 0;
    g_mode_w = PSP_SCREEN_W;
    g_mode_h = PSP_SCREEN_H;
    g_vblank_count = 0;
}

void psp_display_init(void) { psp_display_reset(); }

uint64_t psp_display_vblanks(void) { return g_vblank_count; }
uint64_t psp_display_flips(void) { return g_flips; }
uint32_t psp_display_framebuffer(void) { return g_fb_addr; }

/* Write the current framebuffer as a binary PPM. Returns 0 on success, -1 if
 * no framebuffer has been set yet (which is itself worth knowing during
 * bring-up: the game never got as far as showing anything). */
int psp_display_capture(const char *path) {
    static uint32_t px[PSP_SCREEN_W * PSP_SCREEN_H];
    if (psp_display_read(px) != 0) return -1;

    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", PSP_SCREEN_W, PSP_SCREEN_H);
    for (int i = 0; i < PSP_SCREEN_W * PSP_SCREEN_H; i++) {
        const uint32_t c = px[i];
        uint8_t rgb[3] = { (uint8_t)c, (uint8_t)(c >> 8), (uint8_t)(c >> 16) };
        fwrite(rgb, 1, 3, f);            /* PPM is RGB; alpha is dropped */
    }
    fclose(f);
    return 0;
}

/* The front buffer, converted, for a window or a capture. Read while the game
 * runs: a frame being drawn into the *back* buffer is never seen, which is the
 * point of the game double-buffering. */
int psp_display_read(uint32_t *out) {
    if (psp_render_current()->read_frame) return psp_render_current()->read_frame(out);
    const uint32_t addr = g_fb_addr, stride = g_fb_width, fmt = g_fb_format;
    if (!addr) return -1;
    const int bpp = (fmt == PSP_DISPLAY_PIXEL_FORMAT_8888) ? 4 : 2;
    for (int y = 0; y < PSP_SCREEN_H; y++) {
        for (int x = 0; x < PSP_SCREEN_W; x++) {
            uint32_t at = addr + (uint32_t)(y * (int)stride + x) * (uint32_t)bpp;
            uint32_t px = (bpp == 4) ? psp_read32(at) : psp_read16(at);
            out[y * PSP_SCREEN_W + x] = psp_color_expand(px, (int)fmt);
        }
    }
    return 0;
}

/* ---- the calls ----------------------------------------------------------- */

static void hle_SetMode(void) {
    g_mode   = psp_arg(0);
    g_mode_w = psp_arg(1);
    g_mode_h = psp_arg(2);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_SetFrameBuf(void) {
    /* (topaddr, bufferwidth, pixelformat, sync) */
    if (psp_arg(0) && psp_arg(0) != g_fb_addr) g_flips++;
    g_fb_addr   = psp_arg(0);
    g_fb_width  = psp_arg(1);
    g_fb_format = psp_arg(2);
    if (!g_fb_width) g_fb_width = 512;
    if (g_fb_addr) psp_ge_flip(g_fb_addr, g_fb_width, (int)g_fb_format);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* There is no scanout, so a vblank wait does not wait for one: it bumps the
 * counter and gives the CPU away. A game's main loop is usually
 * `render(); WaitVblank();`, which means this counter is the frame number --
 * the most useful single number to have during bring-up, because it tells you
 * whether the game is looping or stuck -- and the yield is what lets the
 * threads that feed that loop run. */
/* Hold a vblank wait until the next refresh. Nothing else slows a game down, and
 * one that counts frames to time things would otherwise race through them.
 * The rate can change while the game runs; 0 means no pacing. */
static volatile uint32_t g_frame_us;

void psp_display_set_refresh_rate(double hz) {
    g_frame_us = hz > 0 ? (uint32_t)(1000000.0 / hz) : 0;
}

static uint64_t display_now_us(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

/* Every thread waiting for a vblank wakes at the same one, so the next one is
 * worked out from a fixed origin. Handing out turns instead would give each of two
 * waiters every other refresh, halving the frame rate of both. */
static void pace_vblank(uint32_t frame_us) {
    static uint64_t origin;
    uint64_t now = display_now_us();
    if (!origin) origin = now;
    const uint64_t target = origin + ((now - origin) / frame_us + 1) * frame_us;
    while (now < target) {
        psp_threadman_sleep_us(target - now);
        now = display_now_us();
    }
}

static void hle_WaitVblank(void) {
    const uint32_t frame_us = g_frame_us;
    if (frame_us) pace_vblank(frame_us);
    g_vblank_count++;
    /* A frame boundary is the guest asking for time to pass, so it is the third
     * point where an interrupt is delivered (src/hle/intrman.c) -- and, on one
     * CPU, the most natural place to hand the CPU over. */
    psp_threadman_frame_wait();
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_GetVcount(void) { psp_ret((uint32_t)g_vblank_count); }

void psp_display_register(void) {
    psp_hle_register(0x984C27E7, "sceDisplay", "sceDisplayWaitVblankStart",   hle_WaitVblank);
    psp_hle_register(0x9C6EAAD7, "sceDisplay", "sceDisplayGetVcount",         hle_GetVcount);
    psp_hle_register(0x0E20F177, "sceDisplay", "sceDisplaySetMode",           hle_SetMode);
    psp_hle_register(0x289D82FE, "sceDisplay", "sceDisplaySetFrameBuf",       hle_SetFrameBuf);
    psp_hle_register(0x36CDFADE, "sceDisplay", "sceDisplayWaitVblank",        hle_WaitVblank);
    psp_hle_register(0x46F186C3, "sceDisplay", "sceDisplayWaitVblankStartCB", hle_WaitVblank);
}
