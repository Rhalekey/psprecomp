/* psprecomp -- a window to play in.
 *
 * Shows the PSP display, scaled, and turns the keyboard and the first game
 * controller into sceCtrl input. It runs on a host thread of its own and
 * reads the game's front buffer on a 60 Hz timer, so it never waits on the
 * game and the game never waits on it.
 *
 *   Keyboard            Controller (XInput)     PSP
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
 * Windows only for now (Win32 + GDI + XInput: no dependencies). Elsewhere
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

#endif
