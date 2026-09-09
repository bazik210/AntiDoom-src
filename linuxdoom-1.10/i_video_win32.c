#define boolean win_boolean
#include <windows.h>
#undef boolean
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "doomstat.h"
#include "i_system.h"
#include "v_video.h"
#include "m_argv.h"
#include "d_main.h"
#include "doomdef.h"

static HWND win;
static HBITMAP dib;
static HDC dc;
static BITMAPINFO bmi;
static byte palette[768];
static unsigned int *rgb;
static int window_width = 960;
static int window_height = 600;
static boolean closing;
static boolean fullscreen;
int mouse_buttons = 0;
static boolean mouse_captured;
int smooth_scaling = 0;
typedef struct { int sx; int x_diff; int x_diff1; } x_lut_t;
static uint32_t *smooth_buf = NULL;
static int smooth_buf_w = 0, smooth_buf_h = 0;
static x_lut_t *x_lut = NULL;
static int x_lut_w = 0;
static int mouse_accum_x = 0;
static int mouse_accum_y = 0;

void I_CaptureMouse(boolean capture)
{
    if (capture == mouse_captured)
        return;
    mouse_captured = capture;
    if (capture) {
        while (ShowCursor(FALSE) >= 0);
        RECT rc;
        GetClientRect(win, &rc);
        POINT pt = { (rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2 };
        ClientToScreen(win, &pt);
        SetCursorPos(pt.x, pt.y);
        MapWindowPoints(win, NULL, (POINT*)&rc, 2);
        ClipCursor(&rc);
        mouse_accum_x = mouse_accum_y = 0;
    } else {
        ClipCursor(NULL);
        while (ShowCursor(TRUE) < 0);
    }
}

void I_ResetMouse(void)
{
    mouse_accum_x = 0;
    mouse_accum_y = 0;
    if (win && mouse_captured) {
        RECT rc;
        GetClientRect(win, &rc);
        POINT pt = { (rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2 };
        ClientToScreen(win, &pt);
        SetCursorPos(pt.x, pt.y);
    }
}

static void I_UpdateMouseCapture(void)
{
    HWND fg = GetForegroundWindow();
    HWND active = GetActiveWindow();
    extern int menu_mouse;
    boolean is_fg = (fg == win || active == win);
    if (!is_fg && fg) {
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        if (pid == GetCurrentProcessId()) is_fg = true;
    }
    if (fullscreen && !is_fg) {
        SetForegroundWindow(win);
        SetFocus(win);
        is_fg = true;
    }
    boolean want_capture = (is_fg && (fullscreen || !menuactive || !menu_mouse));
    I_CaptureMouse(want_capture);
}

static void I_FillMenuMouseCoords(event_t *ev, LPARAM l)
{
    extern int menu_mouse;
    if (menuactive && menu_mouse) {
        RECT rc; GetClientRect(win, &rc);
        int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
        ev->data2 = (cw > 0) ? (int)LOWORD(l) * BASE_WIDTH / cw : 0;
        ev->data3 = (ch > 0) ? (int)HIWORD(l) * BASE_HEIGHT / ch : 0;
    } else {
        ev->data2 = 0;
        ev->data3 = 0;
    }
}

static int win_key(WPARAM key)
{
    switch (key) {
    case VK_LEFT: return KEY_LEFTARROW;
    case VK_RIGHT: return KEY_RIGHTARROW;
    case VK_UP: return KEY_UPARROW;
    case VK_DOWN: return KEY_DOWNARROW;
    case VK_ESCAPE: return KEY_ESCAPE;
    case VK_RETURN: return KEY_ENTER;
    case VK_TAB: return KEY_TAB;
    case VK_BACK: return KEY_BACKSPACE;
    case VK_SHIFT: return KEY_RSHIFT;
    case VK_CONTROL: return KEY_RCTRL;
    case VK_MENU: return KEY_RALT;
    case VK_F1: return KEY_F1; case VK_F2: return KEY_F2;
    case VK_F3: return KEY_F3; case VK_F4: return KEY_F4;
    case VK_F5: return KEY_F5; case VK_F6: return KEY_F6;
    case VK_F7: return KEY_F7; case VK_F8: return KEY_F8;
    case VK_F9: return KEY_F9; case VK_F10: return KEY_F10;
    case VK_F11: return KEY_F11; case VK_F12: return KEY_F12;
    default:
        if (key >= 'A' && key <= 'Z') return (int)(key - 'A' + 'a');
        if (key >= '0' && key <= '9') return (int)key;
        if (key == VK_SPACE) return ' ';
        return (int)key;
    }
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    event_t ev;
    switch (msg) {
    case WM_INPUT: {
        RAWINPUT raw;
        UINT dwSize = sizeof(RAWINPUT);
        if (GetRawInputData((HRAWINPUT)l, RID_INPUT, &raw, &dwSize, sizeof(RAWINPUTHEADER)) != (UINT)-1) {
            if (raw.header.dwType == RIM_TYPEMOUSE && mouse_captured) {
                mouse_accum_x += raw.data.mouse.lLastX;
                mouse_accum_y += raw.data.mouse.lLastY;
            }
        }
        return DefWindowProc(h, msg, w, l);
    }
    case WM_ACTIVATE:
        if (LOWORD(w) != WA_INACTIVE) {
            I_UpdateMouseCapture();
        } else if (!fullscreen) {
            I_CaptureMouse(false);
        }
        return 0;
    case WM_LBUTTONDOWN: {
        mouse_buttons |= 1;
        extern int menu_mouse;
        if (!mouse_captured && (fullscreen || !menuactive || !menu_mouse)) I_CaptureMouse(true);
        ev.type = ev_mouse; ev.data1 = mouse_buttons;
        I_FillMenuMouseCoords(&ev, l);
        D_PostEvent(&ev); return 0;
    }
    case WM_MOUSEMOVE: {
        extern int menu_mouse;
        if (menuactive && menu_mouse) {
            ev.type = ev_mouse; ev.data1 = mouse_buttons;
            I_FillMenuMouseCoords(&ev, l);
            D_PostEvent(&ev);
            return 0;
        }
        break;
    }
    case WM_LBUTTONUP:
        mouse_buttons &= ~1;
        ev.type = ev_mouse; ev.data1 = mouse_buttons;
        I_FillMenuMouseCoords(&ev, l);
        D_PostEvent(&ev); return 0;
    case WM_RBUTTONDOWN:
        mouse_buttons |= 2;
        ev.type = ev_mouse; ev.data1 = mouse_buttons;
        I_FillMenuMouseCoords(&ev, l);
        D_PostEvent(&ev); return 0;
    case WM_RBUTTONUP:
        mouse_buttons &= ~2;
        ev.type = ev_mouse; ev.data1 = mouse_buttons;
        I_FillMenuMouseCoords(&ev, l);
        D_PostEvent(&ev); return 0;
    case WM_MBUTTONDOWN:
        mouse_buttons |= 4;
        ev.type = ev_mouse; ev.data1 = mouse_buttons;
        I_FillMenuMouseCoords(&ev, l);
        D_PostEvent(&ev); return 0;
    case WM_MBUTTONUP:
        mouse_buttons &= ~4;
        ev.type = ev_mouse; ev.data1 = mouse_buttons;
        I_FillMenuMouseCoords(&ev, l);
        D_PostEvent(&ev); return 0;
    case WM_MOUSEWHEEL: {
        short delta = (short)HIWORD(w);
        if (delta > 0) {
            ev.type = ev_keydown; ev.data1 = KEY_UPARROW; D_PostEvent(&ev);
            ev.type = ev_keyup; D_PostEvent(&ev);
        } else if (delta < 0) {
            ev.type = ev_keydown; ev.data1 = KEY_DOWNARROW; D_PostEvent(&ev);
            ev.type = ev_keyup; D_PostEvent(&ev);
        }
        return 0;
    }
    case WM_SETFOCUS:
        I_UpdateMouseCapture();
        return 0;
    case WM_KEYDOWN:
        if (!(l & (1L << 30))) { ev.type = ev_keydown; ev.data1 = win_key(w); D_PostEvent(&ev); }
        return 0;
    case WM_KEYUP:
        ev.type = ev_keyup; ev.data1 = win_key(w); D_PostEvent(&ev); return 0;
    case WM_KILLFOCUS:
        I_CaptureMouse(false);
        mouse_buttons = 0;
        ev.type = ev_keyup;
        ev.data1 = KEY_RCTRL; D_PostEvent(&ev);
        ev.data1 = KEY_RSHIFT; D_PostEvent(&ev);
        ev.data1 = KEY_RALT; D_PostEvent(&ev);
        ev.data1 = ' '; D_PostEvent(&ev);
        ev.data1 = KEY_LEFTARROW; D_PostEvent(&ev);
        ev.data1 = KEY_RIGHTARROW; D_PostEvent(&ev);
        ev.data1 = KEY_UPARROW; D_PostEvent(&ev);
        ev.data1 = KEY_DOWNARROW; D_PostEvent(&ev);
        return 0;
    case WM_CLOSE:
        I_Quit();
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProc(h, msg, w, l);
}

void I_InitGraphics(void)
{
    WNDCLASS wc;
    RECT r, work;
    MONITORINFO mi;
    int p, w = 960, h = 600;
    boolean maximize = false;
    char *s;

    p = M_CheckParm("-res");
    if (p && p < myargc - 1) sscanf(myargv[p + 1], "%dx%d", &w, &h);
    p = M_CheckParm("-width"); if (p && p < myargc - 1) w = atoi(myargv[p + 1]);
    p = M_CheckParm("-height"); if (p && p < myargc - 1) h = atoi(myargv[p + 1]);
    if (w < 320) w = 320; if (h < 200) h = 200;
    window_width = w; window_height = h;
    fullscreen = M_CheckParm("-fullscreen") != 0;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.lpszClassName = "DoomWin32";
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClass(&wc);

    RAWINPUTDEVICE rid;
    rid.usUsagePage = 0x01;
    rid.usUsage = 0x02;
    rid.dwFlags = 0;
    rid.hwndTarget = NULL;
    RegisterRawInputDevices(&rid, 1, sizeof(rid));

    r.left = 0; r.top = 0; r.right = w; r.bottom = h;
    if (!fullscreen) AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    SystemParametersInfo(SPI_GETWORKAREA, 0, &work, 0);
    mi.cbSize = sizeof(mi);
    GetMonitorInfo(MonitorFromWindow(NULL, MONITOR_DEFAULTTOPRIMARY), &mi);
    if (fullscreen) { w = mi.rcMonitor.right - mi.rcMonitor.left; h = mi.rcMonitor.bottom - mi.rcMonitor.top; }
    else {
        if (M_CheckParm("-maximized") || (r.right-r.left >= work.right-work.left) || (r.bottom-r.top >= work.bottom-work.top))
            maximize = true;
        if (r.right-r.left > work.right-work.left) r.right = r.left + work.right-work.left;
        if (r.bottom-r.top > work.bottom-work.top) r.bottom = r.top + work.bottom-work.top;
    }
    win = CreateWindow("DoomWin32", "AntiDoom - Win32 Port", fullscreen ? WS_POPUP : WS_OVERLAPPEDWINDOW,
                       fullscreen ? mi.rcMonitor.left : work.left + ((work.right-work.left)-(r.right-r.left))/2,
                       fullscreen ? mi.rcMonitor.top : work.top + ((work.bottom-work.top)-(r.bottom-r.top))/2,
                       fullscreen ? w : r.right-r.left, fullscreen ? h : r.bottom-r.top,
                       NULL, NULL, wc.hInstance, NULL);
    if (!win) I_Error("Could not create the Windows window");
    dc = GetDC(win);
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = SCREENWIDTH; bmi.bmiHeader.biHeight = -SCREENHEIGHT;
    bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    rgb = (unsigned int*)malloc(SCREENWIDTH * SCREENHEIGHT * sizeof(unsigned int));
    screens[0] = (byte*)malloc(SCREENWIDTH * SCREENHEIGHT);
    ShowWindow(win, maximize ? SW_MAXIMIZE : SW_SHOW); UpdateWindow(win);
    SetWindowPos(win, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    DWORD curThread = GetCurrentThreadId();
    DWORD fgThread = GetWindowThreadProcessId(GetForegroundWindow(), NULL);
    if (curThread != fgThread) {
        AttachThreadInput(curThread, fgThread, TRUE);
        SetForegroundWindow(win);
        SetFocus(win);
        SetActiveWindow(win);
        AttachThreadInput(curThread, fgThread, FALSE);
    } else {
        SetForegroundWindow(win);
        SetFocus(win);
        SetActiveWindow(win);
    }
    I_UpdateMouseCapture();
    I_Log("Video: %dx%d (%s), Internal Canvas: %dx%d\n", window_width, window_height, fullscreen ? "fullscreen" : "windowed", SCREENWIDTH, SCREENHEIGHT);
    s = getenv("DOOM_WIN32_RES"); (void)s;
}

void I_ShutdownGraphics(void)
{
    I_CaptureMouse(false);
    if (dc) ReleaseDC(win, dc); dc = NULL;
    if (win) DestroyWindow(win); win = NULL;
    free(rgb); rgb = NULL; free(screens[0]); screens[0] = NULL;
    if (smooth_buf) { free(smooth_buf); smooth_buf = NULL; smooth_buf_w = smooth_buf_h = 0; }
    if (x_lut) { free(x_lut); x_lut = NULL; x_lut_w = 0; }
}

void I_StartFrame(void) { }
void I_UpdateNoBlit(void) { }

void I_StartTic(void)
{
    I_UpdateMouseCapture();
    MSG msg;
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) closing = true;
        TranslateMessage(&msg); DispatchMessage(&msg);
    }
    extern boolean level_weapon_ready;
    extern boolean wiping;
    if (wiping || (gamestate == GS_LEVEL && !level_weapon_ready)) {
        mouse_accum_x = 0;
        mouse_accum_y = 0;
    }
    if (mouse_captured) {
        if (mouse_accum_x != 0 || mouse_accum_y != 0) {
            event_t ev;
            ev.type = ev_mouse;
            ev.data1 = mouse_buttons;
            ev.data2 = mouse_accum_x << 2;
            ev.data3 = -mouse_accum_y << 2;
            mouse_accum_x = 0;
            mouse_accum_y = 0;
            D_PostEvent(&ev);
        }

        RECT rc;
        GetClientRect(win, &rc);
        POINT pt = { (rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2 };
        ClientToScreen(win, &pt);
        SetCursorPos(pt.x, pt.y);
    }
}

void I_FinishUpdate(void)
{
    int x, y, left, top, outw, outh;
    RECT r;
    for (y = 0; y < SCREENHEIGHT; ++y)
        for (x = 0; x < SCREENWIDTH; ++x) {
            int i = screens[0][y * SCREENWIDTH + x] * 3;
            rgb[y * SCREENWIDTH + x] = 0xff000000u | ((unsigned)palette[i] << 16) |
                                        ((unsigned)palette[i+1] << 8) | palette[i+2];
        }
    GetClientRect(win, &r); outw = r.right; outh = r.bottom;
    if (M_CheckParm("-keepaspect")) {
        if ((long long)outw * SCREENHEIGHT < (long long)outh * SCREENWIDTH) {
            left = 0; outw = r.right; outh = outw * SCREENHEIGHT / SCREENWIDTH; top = (r.bottom - outh) / 2;
        } else {
            top = 0; outh = r.bottom; outw = outh * SCREENWIDTH / SCREENHEIGHT; left = (r.right - outw) / 2;
        }
    } else {
        left = 0; top = 0; outw = r.right; outh = r.bottom;
    }
    if (smooth_scaling && (outw != SCREENWIDTH || outh != SCREENHEIGHT)) {
        if (outw > smooth_buf_w || outh > smooth_buf_h) {
            if (smooth_buf) free(smooth_buf);
            smooth_buf_w = outw > smooth_buf_w ? outw : smooth_buf_w;
            smooth_buf_h = outh > smooth_buf_h ? outh : smooth_buf_h;
            smooth_buf = (uint32_t*)malloc(smooth_buf_w * smooth_buf_h * sizeof(uint32_t));
        }
        if (outw != x_lut_w) {
            if (x_lut) free(x_lut);
            x_lut = (x_lut_t*)malloc(outw * sizeof(x_lut_t));
            x_lut_w = outw;
            int x_ratio = ((SCREENWIDTH - 1) << 16) / (outw > 1 ? (outw - 1) : 1);
            for (x = 0; x < outw; x++) {
                int val = x * x_ratio;
                x_lut[x].sx = val >> 16;
                x_lut[x].x_diff = (val & 0xffff) >> 8;
                x_lut[x].x_diff1 = 256 - x_lut[x].x_diff;
            }
        }
        int y_ratio = ((SCREENHEIGHT - 1) << 16) / (outh > 1 ? (outh - 1) : 1);
        for (y = 0; y < outh; y++) {
            int y_val = y * y_ratio;
            int sy = y_val >> 16;
            int y_diff = (y_val & 0xffff) >> 8;
            int y_diff1 = 256 - y_diff;
            const uint32_t *s1 = (const uint32_t*)&rgb[sy * SCREENWIDTH];
            const uint32_t *s2 = (const uint32_t*)&rgb[(sy < SCREENHEIGHT - 1 ? sy + 1 : sy) * SCREENWIDTH];
            uint32_t *d_row = &smooth_buf[y * outw];
            for (x = 0; x < outw; x++) {
                int sx = x_lut[x].sx, x_diff = x_lut[x].x_diff, x_diff1 = x_lut[x].x_diff1;
                int sx1 = (sx < SCREENWIDTH - 1) ? sx + 1 : sx;
                uint32_t p00 = s1[sx], p10 = s1[sx1], p01 = s2[sx], p11 = s2[sx1];
                int w00 = (x_diff1 * y_diff1) >> 8, w10 = (x_diff * y_diff1) >> 8;
                int w01 = (x_diff1 * y_diff) >> 8,  w11 = (x_diff * y_diff) >> 8;
                uint32_t rb00 = p00 & 0x00FF00FF, rb10 = p10 & 0x00FF00FF;
                uint32_t rb01 = p01 & 0x00FF00FF, rb11 = p11 & 0x00FF00FF;
                uint32_t rb = ((rb00 * w00 + rb10 * w10 + rb01 * w01 + rb11 * w11) >> 8) & 0x00FF00FF;
                uint32_t ag00 = (p00 >> 8) & 0x00FF00FF, ag10 = (p10 >> 8) & 0x00FF00FF;
                uint32_t ag01 = (p01 >> 8) & 0x00FF00FF, ag11 = (p11 >> 8) & 0x00FF00FF;
                uint32_t ag = (ag00 * w00 + ag10 * w10 + ag01 * w01 + ag11 * w11) & 0xFF00FF00;
                d_row[x] = ag | rb;
            }
        }
        BITMAPINFO s_bmi;
        memset(&s_bmi, 0, sizeof(s_bmi));
        s_bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        s_bmi.bmiHeader.biWidth = outw;
        s_bmi.bmiHeader.biHeight = -outh;
        s_bmi.bmiHeader.biPlanes = 1;
        s_bmi.bmiHeader.biBitCount = 32;
        s_bmi.bmiHeader.biCompression = BI_RGB;
        StretchDIBits(dc, left, top, outw, outh, 0, 0, outw, outh,
                      smooth_buf, &s_bmi, DIB_RGB_COLORS, SRCCOPY);
    } else {
        StretchDIBits(dc, left, top, outw, outh, 0, 0, SCREENWIDTH, SCREENHEIGHT,
                      rgb, &bmi, DIB_RGB_COLORS, SRCCOPY);
    }
    if (top > 0) PatBlt(dc, 0, 0, r.right, top, BLACKNESS);
    if (top + outh < r.bottom) PatBlt(dc, 0, top + outh, r.right, r.bottom - top - outh, BLACKNESS);
    if (left > 0) PatBlt(dc, 0, top, left, outh, BLACKNESS);
    if (left + outw < r.right) PatBlt(dc, left + outw, top, r.right - left - outw, outh, BLACKNESS);
}

void I_ReadScreen(byte *scr) { memcpy(scr, screens[0], SCREENWIDTH * SCREENHEIGHT); }
void I_SetPalette(byte *pal) { memcpy(palette, pal, sizeof(palette)); }
