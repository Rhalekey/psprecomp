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

static uint32_t keyboard(void) {
    static const struct { int vk; uint32_t bit; } K[] = {
        { VK_UP, B_UP }, { VK_RIGHT, B_RIGHT }, { VK_DOWN, B_DOWN }, { VK_LEFT, B_LEFT },
        { 'Z', B_CROSS }, { 'X', B_CIRCLE }, { 'A', B_SQUARE }, { 'S', B_TRIANGLE },
        { 'Q', B_L }, { 'W', B_R }, { VK_RETURN, B_START }, { VK_BACK, B_SELECT },
    };
    uint32_t b = 0;
    for (size_t i = 0; i < sizeof K / sizeof K[0]; i++)
        if (GetAsyncKeyState(K[i].vk) & 0x8000) b |= K[i].bit;
    return b;
}

/* Asking an empty slot is slow enough to hurt if done every frame, so while nothing
 * is connected the slots are tried in turn, one a second or so. */
#define PAD_SLOTS 4
#define PAD_RETRY_TICKS 60

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
 * downwards, and both run -32768..32767. */
static void stick(int x, int y, uint8_t *ax, uint8_t *ay) {
    const long long d = XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE;
    if ((long long)x * x + (long long)y * y <= d * d) return;
    *ax = (uint8_t)((x + 32768) >> 8);
    *ay = (uint8_t)((y + 32768) >> 8);
}

/* The first XInput controller connected, if any. */
static uint32_t xinput_pad(uint8_t *ax, uint8_t *ay) {
    static const struct { WORD x; uint32_t bit; } P[] = {
        { XINPUT_GAMEPAD_DPAD_UP, B_UP }, { XINPUT_GAMEPAD_DPAD_RIGHT, B_RIGHT },
        { XINPUT_GAMEPAD_DPAD_DOWN, B_DOWN }, { XINPUT_GAMEPAD_DPAD_LEFT, B_LEFT },
        { XINPUT_GAMEPAD_A, B_CROSS }, { XINPUT_GAMEPAD_B, B_CIRCLE },
        { XINPUT_GAMEPAD_X, B_SQUARE }, { XINPUT_GAMEPAD_Y, B_TRIANGLE },
        { XINPUT_GAMEPAD_LEFT_SHOULDER, B_L }, { XINPUT_GAMEPAD_RIGHT_SHOULDER, B_R },
        { XINPUT_GAMEPAD_START, B_START }, { XINPUT_GAMEPAD_BACK, B_SELECT },
    };
    XINPUT_STATE s;
    if (!read_pad(&s)) return 0;
    uint32_t b = 0;
    for (size_t i = 0; i < sizeof P / sizeof P[0]; i++)
        if (s.Gamepad.wButtons & P[i].x) b |= P[i].bit;
    if (s.Gamepad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) b |= B_L;
    if (s.Gamepad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) b |= B_R;
    stick(s.Gamepad.sThumbLX, -1 - s.Gamepad.sThumbLY, ax, ay);
    return b;
}

/* SDL3 knows the other controllers: PlayStation, Nintendo and many more. It is an optional
 * DLL beside the executable, so that without it the game still runs, on XInput. Everything
 * below comes from SDL3's stable interface. */
enum {
    SDL_INIT_GAMEPAD_ = 0x2000,
    SDL_SOUTH = 0, SDL_EAST = 1, SDL_WEST = 2, SDL_NORTH = 3, SDL_BACK = 4, SDL_START = 6,
    SDL_LEFT_SHOULDER = 9, SDL_RIGHT_SHOULDER = 10,
    SDL_DPAD_UP = 11, SDL_DPAD_DOWN = 12, SDL_DPAD_LEFT = 13, SDL_DPAD_RIGHT = 14,
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
    static const struct { int button; uint32_t bit; } P[] = {
        { SDL_DPAD_UP, B_UP }, { SDL_DPAD_RIGHT, B_RIGHT },
        { SDL_DPAD_DOWN, B_DOWN }, { SDL_DPAD_LEFT, B_LEFT },
        { SDL_SOUTH, B_CROSS }, { SDL_EAST, B_CIRCLE },
        { SDL_WEST, B_SQUARE }, { SDL_NORTH, B_TRIANGLE },
        { SDL_LEFT_SHOULDER, B_L }, { SDL_RIGHT_SHOULDER, B_R },
        { SDL_START, B_START }, { SDL_BACK, B_SELECT },
    };
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
    uint32_t b = 0;
    for (int n = 0; n < open_count; n++) {
        void *gamepad = open[n].gamepad;
        for (size_t i = 0; i < sizeof P / sizeof P[0]; i++)
            if (sdl.GetGamepadButton(gamepad, P[i].button)) b |= P[i].bit;
        if (sdl.GetGamepadAxis(gamepad, SDL_AXIS_LEFT_TRIGGER) > SDL_TRIGGER_DOWN) b |= B_L;
        if (sdl.GetGamepadAxis(gamepad, SDL_AXIS_RIGHT_TRIGGER) > SDL_TRIGGER_DOWN) b |= B_R;
        stick(sdl.GetGamepadAxis(gamepad, SDL_AXIS_LEFTX), sdl.GetGamepadAxis(gamepad, SDL_AXIS_LEFTY), ax, ay);
    }
    return b;
}

static uint32_t pad(uint8_t *ax, uint8_t *ay) {
    return sdl_ready() ? sdl_pad(ax, ay) : xinput_pad(ax, ay);
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

static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER: {
        uint8_t ax = 128, ay = 128;
        uint32_t b = pad(&ax, &ay);
        if (GetForegroundWindow() == h) b |= keyboard();   /* keys only when focused */
        psp_ctrl_set(b, ax, ay);
        InterlockedExchange(&g_buttons, (LONG)b);
        if (!g_external) InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_PAINT:      paint(h); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_DESTROY:    PostQuitMessage(0); return 0;
    default:            return DefWindowProcA(h, msg, wp, lp);
    }
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

#endif
