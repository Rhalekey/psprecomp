/* psprecomp -- a window to play in. See include/psprecomp/window.h. */

#include "psprecomp/window.h"
#include "psprecomp/hle.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>

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

/* The first XInput controller, if one is connected. */
static uint32_t pad(uint8_t *ax, uint8_t *ay) {
    static const struct { WORD x; uint32_t bit; } P[] = {
        { XINPUT_GAMEPAD_DPAD_UP, B_UP }, { XINPUT_GAMEPAD_DPAD_RIGHT, B_RIGHT },
        { XINPUT_GAMEPAD_DPAD_DOWN, B_DOWN }, { XINPUT_GAMEPAD_DPAD_LEFT, B_LEFT },
        { XINPUT_GAMEPAD_A, B_CROSS }, { XINPUT_GAMEPAD_B, B_CIRCLE },
        { XINPUT_GAMEPAD_X, B_SQUARE }, { XINPUT_GAMEPAD_Y, B_TRIANGLE },
        { XINPUT_GAMEPAD_LEFT_SHOULDER, B_L }, { XINPUT_GAMEPAD_RIGHT_SHOULDER, B_R },
        { XINPUT_GAMEPAD_START, B_START }, { XINPUT_GAMEPAD_BACK, B_SELECT },
    };
    XINPUT_STATE s;
    if (XInputGetState(0, &s) != ERROR_SUCCESS) return 0;
    uint32_t b = 0;
    for (size_t i = 0; i < sizeof P / sizeof P[0]; i++)
        if (s.Gamepad.wButtons & P[i].x) b |= P[i].bit;
    if (s.Gamepad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) b |= B_L;
    if (s.Gamepad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) b |= B_R;
    /* The PSP stick is 0..255 with 128 at rest and y growing downwards. */
    const int lx = s.Gamepad.sThumbLX, ly = s.Gamepad.sThumbLY;
    if (lx * lx + ly * ly > XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE * XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) {
        *ax = (uint8_t)((lx + 32768) >> 8);
        *ay = (uint8_t)((32767 - ly) >> 8);
    }
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

static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER: {
        uint8_t ax = 128, ay = 128;
        uint32_t b = pad(&ax, &ay);
        if (GetForegroundWindow() == h) b |= keyboard();   /* keys only when focused */
        psp_ctrl_set(b, ax, ay);
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

#else

/* ponytail: no window off Windows yet; SDL2, as ps3recomp uses there, is the
 * obvious next step. */
int psp_window_start(const char *title, int scale) { (void)title; (void)scale; return -1; }
int psp_window_closed(void) { return 0; }
void *psp_window_handle(void) { return 0; }
void psp_window_set_external(int on) { (void)on; }

#endif
