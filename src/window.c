/* psprecomp -- a window to play in. See include/psprecomp/window.h. */

#include "psprecomp/window.h"
#include "psprecomp/hle.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>
#include <stdio.h>

#define W 480
#define H 272

/* PSP button bits (SceCtrlData.Buttons). */
enum {
    B_SELECT = 0x0001, B_START = 0x0008,
    B_UP = 0x0010, B_RIGHT = 0x0020, B_DOWN = 0x0040, B_LEFT = 0x0080,
    B_L = 0x0100, B_R = 0x0200,
    B_TRIANGLE = 0x1000, B_CIRCLE = 0x2000, B_CROSS = 0x4000, B_SQUARE = 0x8000
};

static volatile LONG g_closed;
static volatile LONG g_external;     /* something else presents to the window */
static volatile LONG g_buttons;
static HWND g_hwnd;
static int g_scale;
static const char *g_title = "psprecomp";
static uint32_t g_px[W * H];

/* In the order of PSP_BIND_*. */
static const uint32_t BUTTON_BIT[PSP_BIND_COUNT] = {
    B_UP, B_RIGHT, B_DOWN, B_LEFT, B_CROSS, B_CIRCLE, B_SQUARE, B_TRIANGLE, B_L, B_R, B_START, B_SELECT,
};
#define PAD_BIT(id) ((uint32_t)1 << (id))
#define DEFAULT_KEY_LIST { VK_UP, VK_RIGHT, VK_DOWN, VK_LEFT, 'Z', 'X', 'A', 'S', 'Q', 'W', VK_RETURN, VK_BACK }
#define DEFAULT_PAD_LIST { \
    PAD_BIT(11), PAD_BIT(14), PAD_BIT(12), PAD_BIT(13), PAD_BIT(0), PAD_BIT(1), PAD_BIT(2), PAD_BIT(3), \
    PAD_BIT(9) | PAD_BIT(PSP_PAD_LT), PAD_BIT(10) | PAD_BIT(PSP_PAD_RT), PAD_BIT(6), PAD_BIT(4) }
static const int DEFAULT_KEYS[PSP_BIND_COUNT] = DEFAULT_KEY_LIST;
static const uint32_t DEFAULT_PADS[PSP_BIND_COUNT] = DEFAULT_PAD_LIST;
static volatile int g_keys[PSP_BIND_COUNT] = DEFAULT_KEY_LIST;
static volatile int g_keys2[PSP_BIND_COUNT];
static volatile uint32_t g_pads[PSP_BIND_COUNT] = DEFAULT_PAD_LIST;
static volatile uint32_t g_pad_raw;

void psp_window_set_key(int button, int vk) { g_keys[button] = vk; }
int psp_window_get_key(int button) { return g_keys[button]; }
void psp_window_set_key2(int button, int vk) { g_keys2[button] = vk; }
int psp_window_get_key2(int button) { return g_keys2[button]; }
void psp_window_set_pad(int button, uint32_t mask) { g_pads[button] = mask; }
uint32_t psp_window_get_pad(int button) { return g_pads[button]; }
uint32_t psp_window_pad_raw(void) { return g_pad_raw; }

void psp_window_reset_buttons(void) {
    for (int i = 0; i < PSP_BIND_COUNT; i++) {
        g_keys[i] = DEFAULT_KEYS[i];
        g_keys2[i] = 0;
        g_pads[i] = DEFAULT_PADS[i];
    }
}

static uint32_t keyboard(void) {
    uint32_t b = 0;
    /* Alt and the Windows key go with shortcuts: Alt+Enter is not Start, Win+Shift+S not Triangle. */
    if ((GetAsyncKeyState(VK_MENU) | GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) return 0;
    for (int i = 0; i < PSP_BIND_COUNT; i++)
        if ((g_keys[i] && (GetAsyncKeyState(g_keys[i]) & 0x8000)) || (g_keys2[i] && (GetAsyncKeyState(g_keys2[i]) & 0x8000))) b |= BUTTON_BIT[i];
    return b;
}

/* Asking an empty slot is slow enough to hurt if done every frame, so while nothing
 * is connected the slots are tried in turn, one a second or so. */
#define PAD_SLOTS 4
#define PAD_RETRY_TICKS 1000

static int read_pad(XINPUT_STATE *s) {
    static int slot = -1, next_slot, wait;
    if (slot >= 0) {
        if (XInputGetState((DWORD)slot, s) == ERROR_SUCCESS) return 1;
        fprintf(stderr, "window: controller %d disconnected\n", slot);
        slot = -1;
    }
    if (wait-- > 0) return 0;
    wait = PAD_RETRY_TICKS / PAD_SLOTS;
    const int trying = next_slot;
    next_slot = (next_slot + 1) % PAD_SLOTS;
    if (XInputGetState((DWORD)trying, s) != ERROR_SUCCESS) return 0;
    slot = trying;
    fprintf(stderr, "window: controller found in slot %d\n", slot);
    return 1;
}

/* The PSP stick is 0..255 with 128 at rest and y growing downwards. Here too y grows
 * downwards, and both run -32768..32767. Pushed past half way, the stick is the d-pad as well,
 * as the game's menus and notes read only that: its buttons are given back, by SDL's numbers. */
#define STICK_AS_DPAD 16384
static uint32_t g_stick_raw;                 /* the d-pad's buttons that came from a stick, this read */
static volatile uint32_t g_stick_buttons;    /* the same as the PSP's buttons */
uint32_t psp_window_stick_buttons(void) { return g_stick_buttons; }

static uint32_t stick(int x, int y, uint8_t *ax, uint8_t *ay) {
    const long long d = XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE;
    if ((long long)x * x + (long long)y * y <= d * d) return 0;
    *ax = (uint8_t)((x + 32768) >> 8);
    *ay = (uint8_t)((y + 32768) >> 8);
    uint32_t dpad = 0;
    if (y < -STICK_AS_DPAD) dpad |= PAD_BIT(11);
    if (y > STICK_AS_DPAD) dpad |= PAD_BIT(12);
    if (x < -STICK_AS_DPAD) dpad |= PAD_BIT(13);
    if (x > STICK_AS_DPAD) dpad |= PAD_BIT(14);
    g_stick_raw |= dpad;
    return dpad;
}

/* The first XInput controller connected, if any. Its buttons get SDL's numbers. */
static uint32_t xinput_pad(uint8_t *ax, uint8_t *ay) {
    static const struct { WORD x; int id; } P[] = {
        { XINPUT_GAMEPAD_DPAD_UP, 11 }, { XINPUT_GAMEPAD_DPAD_RIGHT, 14 },
        { XINPUT_GAMEPAD_DPAD_DOWN, 12 }, { XINPUT_GAMEPAD_DPAD_LEFT, 13 },
        { XINPUT_GAMEPAD_A, 0 }, { XINPUT_GAMEPAD_B, 1 },
        { XINPUT_GAMEPAD_X, 2 }, { XINPUT_GAMEPAD_Y, 3 },
        { XINPUT_GAMEPAD_LEFT_SHOULDER, 9 }, { XINPUT_GAMEPAD_RIGHT_SHOULDER, 10 },
        { XINPUT_GAMEPAD_START, 6 }, { XINPUT_GAMEPAD_BACK, 4 },
        { XINPUT_GAMEPAD_LEFT_THUMB, 7 }, { XINPUT_GAMEPAD_RIGHT_THUMB, 8 },
    };
    XINPUT_STATE s;
    if (!read_pad(&s)) return 0;
    uint32_t raw = 0;
    for (size_t i = 0; i < sizeof P / sizeof P[0]; i++)
        if (s.Gamepad.wButtons & P[i].x) raw |= PAD_BIT(P[i].id);
    if (s.Gamepad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) raw |= PAD_BIT(PSP_PAD_LT);
    if (s.Gamepad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) raw |= PAD_BIT(PSP_PAD_RT);
    raw |= stick(s.Gamepad.sThumbLX, -1 - s.Gamepad.sThumbLY, ax, ay);
    return raw;
}

/* SDL3 knows the other controllers: PlayStation, Nintendo and many more. It is an optional
 * DLL beside the executable, so that without it the game still runs, on XInput. Everything
 * below comes from SDL3's stable interface. */
enum {
    SDL_INIT_GAMEPAD_ = 0x2000,
    SDL_BUTTONS = 21,
    SDL_AXIS_LEFTX = 0, SDL_AXIS_LEFTY = 1, SDL_AXIS_LEFT_TRIGGER = 4, SDL_AXIS_RIGHT_TRIGGER = 5,
    SDL_TRIGGER_DOWN = 30 * 128,        /* XInput's threshold, on SDL's scale */
    SDL_MAX_PADS = 8
};

static struct {
    _Bool (*Init)(uint32_t);
    _Bool (*SetHint)(const char *, const char *);
    const char *(*GetError)(void);
    void (*PumpEvents)(void);
    void (*FlushEvents)(uint32_t, uint32_t);
    uint32_t *(*GetGamepads)(int *);
    void *(*OpenGamepad)(uint32_t);
    void (*CloseGamepad)(void *);
    _Bool (*GamepadConnected)(void *);
    const char *(*GetGamepadName)(void *);
    _Bool (*GetGamepadButton)(void *, int);
    int16_t (*GetGamepadAxis)(void *, int);
    void (*free)(void *);
} sdl;

/* Runs on the window's thread, as all of SDL's use here does. */
static int sdl_ready(void) {
    static int state;                   /* 0 not tried, 1 ready, -1 not available */
    if (state) return state > 0;
    state = -1;
    /* Only from beside the executable, never from wherever else Windows would look.
     * System32 is for what SDL itself needs, which these flags also govern. */
    HMODULE m = LoadLibraryExA("SDL3.dll", NULL, LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m) {
        fprintf(stderr, "window: SDL3.dll did not load (error %lu), only XInput controllers will work\n", GetLastError());
        return 0;
    }
#define SDL_FN(field, name) \
    if (!(*(FARPROC *)&sdl.field = GetProcAddress(m, name))) { \
        fprintf(stderr, "window: SDL3.dll has no %s, only XInput controllers will work\n", name); \
        return 0; \
    }
    SDL_FN(Init, "SDL_Init") SDL_FN(SetHint, "SDL_SetHint") SDL_FN(GetError, "SDL_GetError")
    SDL_FN(PumpEvents, "SDL_PumpEvents") SDL_FN(FlushEvents, "SDL_FlushEvents")
    SDL_FN(GetGamepads, "SDL_GetGamepads") SDL_FN(OpenGamepad, "SDL_OpenGamepad")
    SDL_FN(CloseGamepad, "SDL_CloseGamepad") SDL_FN(GamepadConnected, "SDL_GamepadConnected")
    SDL_FN(GetGamepadName, "SDL_GetGamepadName") SDL_FN(GetGamepadButton, "SDL_GetGamepadButton")
    SDL_FN(GetGamepadAxis, "SDL_GetGamepadAxis") SDL_FN(free, "SDL_free")
#undef SDL_FN
    /* SDL has no window of its own here, so it must not wait for one to have focus. */
    sdl.SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
    if (!sdl.Init(SDL_INIT_GAMEPAD_)) {
        fprintf(stderr, "window: SDL3 did not start (%s), only XInput controllers will work\n", sdl.GetError());
        return 0;
    }
    state = 1;
    return 1;
}

/* Every controller SDL can see, all playing as one. Buttons go by where they sit and not by
 * what is printed on them, so the bottom one is Cross everywhere, B on a Nintendo controller. */
static uint32_t sdl_pad(uint8_t *ax, uint8_t *ay) {
    static struct { void *gamepad; uint32_t id; } open[SDL_MAX_PADS];
    static int open_count, wait;
    sdl.PumpEvents();
    sdl.FlushEvents(0, 0xFFFFFFFFu);    /* nothing reads them, and they would pile up */
    for (int i = open_count - 1; i >= 0; i--) {
        if (sdl.GamepadConnected(open[i].gamepad)) continue;
        fprintf(stderr, "window: controller disconnected\n");
        sdl.CloseGamepad(open[i].gamepad);
        open[i] = open[--open_count];
    }
    if (wait-- <= 0) {
        wait = PAD_RETRY_TICKS;
        int count = 0;
        uint32_t *ids = sdl.GetGamepads(&count);
        for (int i = 0; ids && i < count && open_count < SDL_MAX_PADS; i++) {
            int known = 0;
            for (int k = 0; k < open_count; k++) known |= open[k].id == ids[i];
            if (known) continue;
            void *gamepad = sdl.OpenGamepad(ids[i]);
            if (!gamepad) continue;
            fprintf(stderr, "window: controller found: %s\n", sdl.GetGamepadName(gamepad));
            open[open_count].gamepad = gamepad;
            open[open_count++].id = ids[i];
        }
        if (ids) sdl.free(ids);
    }
    uint32_t raw = 0;
    for (int n = 0; n < open_count; n++) {
        void *gamepad = open[n].gamepad;
        for (int id = 0; id < SDL_BUTTONS; id++)
            if (sdl.GetGamepadButton(gamepad, id)) raw |= PAD_BIT(id);
        if (sdl.GetGamepadAxis(gamepad, SDL_AXIS_LEFT_TRIGGER) > SDL_TRIGGER_DOWN) raw |= PAD_BIT(PSP_PAD_LT);
        if (sdl.GetGamepadAxis(gamepad, SDL_AXIS_RIGHT_TRIGGER) > SDL_TRIGGER_DOWN) raw |= PAD_BIT(PSP_PAD_RT);
        raw |= stick(sdl.GetGamepadAxis(gamepad, SDL_AXIS_LEFTX), sdl.GetGamepadAxis(gamepad, SDL_AXIS_LEFTY), ax, ay);
    }
    return raw;
}

static uint32_t pad(uint8_t *ax, uint8_t *ay) {
    g_stick_raw = 0;
    const uint32_t raw = sdl_ready() ? sdl_pad(ax, ay) : xinput_pad(ax, ay);
    g_pad_raw = raw;
    uint32_t b = 0, from_stick = 0;
    for (int i = 0; i < PSP_BIND_COUNT; i++) {
        if (raw & g_pads[i]) b |= BUTTON_BIT[i];
        /* Only if the stick alone gives it: a d-pad held the same way counts as the d-pad. */
        if ((g_stick_raw & g_pads[i]) && !(raw & ~g_stick_raw & g_pads[i])) from_stick |= BUTTON_BIT[i];
    }
    g_stick_buttons = from_stick;
    return b;
}

static void paint(HWND h) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    if (g_external) { EndPaint(h, &ps); return; }
    RECT r;
    GetClientRect(h, &r);
    const int cw = r.right, ch = r.bottom;
    /* Keep the PSP's aspect, black bars around it. */
    int w = cw, hh = cw * H / W;
    if (hh > ch) { hh = ch; w = ch * W / H; }
    const int x0 = (cw - w) / 2, y0 = (ch - hh) / 2;
    PatBlt(dc, 0, 0, cw, y0, BLACKNESS);
    PatBlt(dc, 0, y0 + hh, cw, ch - y0 - hh, BLACKNESS);
    PatBlt(dc, 0, 0, x0, ch, BLACKNESS);
    PatBlt(dc, x0 + w, 0, cw - x0 - w, ch, BLACKNESS);

    if (psp_display_read(g_px) == 0) {
        for (int i = 0; i < W * H; i++) {      /* 0xAABBGGRR to the DIB's BGRA */
            const uint32_t c = g_px[i];
            g_px[i] = (c & 0xFF00FF00u) | ((c & 0xFFu) << 16) | ((c >> 16) & 0xFFu);
        }
        BITMAPINFO bi;
        ZeroMemory(&bi, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = W;
        bi.bmiHeader.biHeight = -H;            /* top-down */
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dc, COLORONCOLOR);   /* nearest: crisp pixels */
        StretchDIBits(dc, x0, y0, w, hh, 0, 0, W, H, g_px, &bi, DIB_RGB_COLORS, SRCCOPY);
    } else {
        PatBlt(dc, x0, y0, w, hh, BLACKNESS);
    }
    EndPaint(h, &ps);
}

/* The whole of the window's monitor with no borders, and back to the window as it was. Asked
 * from any thread, and done on the window's own. */
static volatile LONG g_mode_asked = PSP_WINDOW_NORMAL;
static int g_mode = PSP_WINDOW_NORMAL;
static int g_last_covering = PSP_WINDOW_FULL_SCREEN;

void psp_window_set_mode(int mode) {
    if (mode >= 0 && mode < PSP_WINDOW_MODE_COUNT) InterlockedExchange(&g_mode_asked, mode);
}

int psp_window_get_mode(void) { return (int)g_mode_asked; }

static void take_up_mode(HWND h) {
    static WINDOWPLACEMENT before;
    const int mode = (int)g_mode_asked;
    if (mode == g_mode) return;
    if (g_mode == PSP_WINDOW_NORMAL) {
        before.length = sizeof before;
        GetWindowPlacement(h, &before);
    }
    if (mode == PSP_WINDOW_NORMAL) {
        SetWindowLongPtrA(h, GWL_STYLE, (GetWindowLongPtrA(h, GWL_STYLE) & ~(LONG_PTR)WS_POPUP) | WS_OVERLAPPEDWINDOW);
        if (before.length) SetWindowPlacement(h, &before);
        SetWindowPos(h, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        MONITORINFO info;
        info.cbSize = sizeof info;
        if (!GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &info)) return;
        const RECT m = info.rcMonitor;
        /* A window that covers the screen exactly is what drivers take for a full screen game.
         * One row past it, out of sight, it stays a window of the desktop. */
        const int extra = mode == PSP_WINDOW_BORDERLESS ? 1 : 0;
        g_last_covering = mode;
        SetWindowLongPtrA(h, GWL_STYLE, (GetWindowLongPtrA(h, GWL_STYLE) & ~(LONG_PTR)WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(h, HWND_TOP, m.left, m.top, m.right - m.left, m.bottom - m.top + extra, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    }
    g_mode = mode;
}

/* Looked for here, as a screen of the host's own can hold the keyboard's focus. */
static void full_screen_keys(HWND h) {
    static int was_down = 1;
    const int focused = GetForegroundWindow() == h;
    const int down = focused && ((GetAsyncKeyState(VK_F11) & 0x8000) ||
                                 ((GetAsyncKeyState(VK_MENU) & 0x8000) && (GetAsyncKeyState(VK_RETURN) & 0x8000)));
    if (down && !was_down) psp_window_set_mode(g_mode == PSP_WINDOW_NORMAL ? g_last_covering : PSP_WINDOW_NORMAL);
    was_down = down;
    take_up_mode(h);
}

static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER:
        full_screen_keys(h);
        if (!g_external) InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_PAINT:      paint(h); return 0;
    case WM_ERASEBKGND: return 1;
    /* No beep for Alt+Enter. */
    case WM_SYSCHAR:    if (wp == VK_RETURN) return 0; return DefWindowProcA(h, msg, wp, lp);
    case WM_DESTROY:    PostQuitMessage(0); return 0;
    default:            return DefWindowProcA(h, msg, wp, lp);
    }
}

/* The buttons, about once a millisecond. A window's timer comes every 16 at best and later when
 * the window is busy, which a rhythm game feels. Whatever SDL may have made on this thread gets
 * its messages here. */
static DWORD WINAPI input_main(LPVOID arg) {
    HWND h = (HWND)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    while (!g_closed) {
        MSG m;
        while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageA(&m);
        }
        uint8_t ax = 128, ay = 128;
        uint32_t b = pad(&ax, &ay);
        if (GetForegroundWindow() == h) b |= keyboard();   /* keys only when focused */
        psp_ctrl_set(b, ax, ay);
        InterlockedExchange(&g_buttons, (LONG)b);
        Sleep(1);
    }
    return 0;
}

static DWORD WINAPI window_main(LPVOID arg) {
    HANDLE ready = (HANDLE)arg;
    WNDCLASSA wc;
    ZeroMemory(&wc, sizeof wc);
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "psprecomp_window";
    RegisterClassA(&wc);

    /* Per-monitor pixels, not Windows' blurry bitmap stretch of a DPI-unaware
     * window. With no scale asked for, take the largest whole multiple that
     * fits most of the screen's height. */
    SetProcessDPIAware();
    if (g_scale <= 0) {
        RECT wa;
        SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);
        g_scale = (int)((wa.bottom - wa.top) * 7 / 10 / H);
        if (g_scale < 1) g_scale = 1;
    }
    RECT r = { 0, 0, W * g_scale, H * g_scale };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND h = CreateWindowA(wc.lpszClassName, g_title, WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                           NULL, NULL, wc.hInstance, NULL);
    if (!h) { InterlockedExchange(&g_closed, 1); SetEvent(ready); return 1; }
    g_hwnd = h;
    SetTimer(h, 1, 16, NULL);
    HANDLE input = CreateThread(NULL, 0, input_main, h, 0, NULL);
    if (input) CloseHandle(input);
    else fprintf(stderr, "window: no thread for the buttons\n");
    SetEvent(ready);

    MSG m;
    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    InterlockedExchange(&g_closed, 1);
    return 0;
}

int psp_window_start(const char *title, int scale) {
    if (title) g_title = title;
    g_scale = scale;
    HANDLE ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    HANDLE t = CreateThread(NULL, 0, window_main, ready, 0, NULL);
    if (!t) { CloseHandle(ready); return -1; }
    WaitForSingleObject(ready, INFINITE);       /* the window exists, or failed */
    CloseHandle(ready);
    CloseHandle(t);
    return g_closed ? -1 : 0;
}

int psp_window_closed(void) { return g_closed != 0; }
void *psp_window_handle(void) { return g_hwnd; }
void psp_window_set_external(int on) { InterlockedExchange(&g_external, on); }
uint32_t psp_window_buttons(void) { return (uint32_t)g_buttons; }

#else

/* ponytail: no window off Windows yet; SDL2, as ps3recomp uses there, is the
 * obvious next step. */
int psp_window_start(const char *title, int scale) { (void)title; (void)scale; return -1; }
int psp_window_closed(void) { return 0; }
void *psp_window_handle(void) { return 0; }
void psp_window_set_external(int on) { (void)on; }
uint32_t psp_window_buttons(void) { return 0; }
void psp_window_set_key(int button, int vk) { (void)button; (void)vk; }
int psp_window_get_key(int button) { (void)button; return 0; }
void psp_window_set_key2(int button, int vk) { (void)button; (void)vk; }
int psp_window_get_key2(int button) { (void)button; return 0; }
void psp_window_set_pad(int button, uint32_t mask) { (void)button; (void)mask; }
uint32_t psp_window_get_pad(int button) { (void)button; return 0; }
void psp_window_reset_buttons(void) {}
uint32_t psp_window_pad_raw(void) { return 0; }
uint32_t psp_window_stick_buttons(void) { return 0; }

#endif
