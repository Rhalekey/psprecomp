/* psprecomp -- a window to play in.
 *
 * Shows the PSP display, scaled, and turns the keyboard and game controllers
 * into sceCtrl input. It runs on a host thread of its own and
 * reads the game's front buffer on a 60 Hz timer, so it never waits on the
 * game and the game never waits on it.
 *
 *   Keyboard            Controller (Xbox names) PSP
 *   arrows              d-pad                   d-pad
 *   (none)              left stick              analog stick
 *   Z                   A                       Cross
 *   X                   B                       Circle
 *   A                   X                       Square
 *   S                   Y                       Triangle
 *   Q / W               LB, LT / RB, RT         L / R
 *   Enter               Start                   Start
 *   Backspace           Back                    Select
 *
 * The face buttons go by position, so the bottom one is Cross on any controller.
 * With SDL3.dll beside the executable, all connected controllers SDL knows work
 * at once, PlayStation and Nintendo included; without it, the first XInput one does.
 *
 * All of these can be changed while running, below.
 *
 * Windows only for now (Win32 + GDI + XInput, no build dependencies). Elsewhere
 * psp_window_start() returns -1. */
#ifndef PSPRECOMP_WINDOW_H
#define PSPRECOMP_WINDOW_H

#include <stdint.h>

/* Open the window, `scale` times the PSP's 480x272, or with `scale` 0 as
 * large as fits the screen. Returns 0 if it opened. */
int psp_window_start(const char *title, int scale);

/* Nonzero once the user has closed the window. */
int psp_window_closed(void);

/* The native window, for a render backend that presents to it directly, and whether
 * one does. While set, the window stops drawing the front buffer itself. */
void *psp_window_handle(void);
void psp_window_set_external(int on);

/* The PSP buttons held right now, for a screen shown before the game runs. */
uint32_t psp_window_buttons(void);

/* Which key and which controller buttons make each PSP button, numbered as here. A key is a
 * Windows virtual key, and a controller button a bit of what psp_window_pad_raw() gives. */
enum {
    PSP_BIND_UP, PSP_BIND_RIGHT, PSP_BIND_DOWN, PSP_BIND_LEFT, PSP_BIND_CROSS, PSP_BIND_CIRCLE,
    PSP_BIND_SQUARE, PSP_BIND_TRIANGLE, PSP_BIND_L, PSP_BIND_R, PSP_BIND_START, PSP_BIND_SELECT,
    PSP_BIND_COUNT
};
/* The triggers, past what SDL3 numbers as buttons. */
enum { PSP_PAD_LT = 21, PSP_PAD_RT = 22 };

void psp_window_set_key(int button, int vk);
int psp_window_get_key(int button);
void psp_window_set_pad(int button, uint32_t mask);
uint32_t psp_window_get_pad(int button);
void psp_window_reset_buttons(void);

/* Every controller button held, one bit each, by the numbers SDL3 gives them. */
uint32_t psp_window_pad_raw(void);

#endif
