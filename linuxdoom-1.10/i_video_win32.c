#define boolean win_boolean
#include <windows.h>
#undef boolean
#include <stdlib.h>
#include <string.h>

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
    case WM_KEYDOWN:
        if (!(l & (1L << 30))) { ev.type = ev_keydown; ev.data1 = win_key(w); D_PostEvent(&ev); }
        return 0;
    case WM_KEYUP:
        ev.type = ev_keyup; ev.data1 = win_key(w); D_PostEvent(&ev); return 0;
    case WM_CLOSE:
        closing = true; ev.type = ev_keydown; ev.data1 = KEY_ESCAPE; D_PostEvent(&ev); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProc(h, msg, w, l);
}

void I_InitGraphics(void)
{
    WNDCLASS wc;
    RECT r;
    int p, w = 960, h = 600;
    char *s;

    p = M_CheckParm("-res");
    if (p && p < myargc - 1) sscanf(myargv[p + 1], "%dx%d", &w, &h);
    p = M_CheckParm("-width"); if (p && p < myargc - 1) w = atoi(myargv[p + 1]);
    p = M_CheckParm("-height"); if (p && p < myargc - 1) h = atoi(myargv[p + 1]);
    if (w < 320) w = 320; if (h < 200) h = 200;
    window_width = w; window_height = h;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.lpszClassName = "DoomWin32";
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClass(&wc);
    r.left = 0; r.top = 0; r.right = w; r.bottom = h;
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    win = CreateWindow("DoomWin32", "DOOM II - Win32 port", WS_OVERLAPPEDWINDOW,
                       CW_USEDEFAULT, CW_USEDEFAULT, r.right-r.left, r.bottom-r.top,
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
    ShowWindow(win, SW_SHOW); UpdateWindow(win);
    s = getenv("DOOM_WIN32_RES"); (void)s;
}

void I_ShutdownGraphics(void)
{
    if (dc) ReleaseDC(win, dc); dc = NULL;
    if (win) DestroyWindow(win); win = NULL;
    free(rgb); rgb = NULL; free(screens[0]); screens[0] = NULL;
}

void I_StartFrame(void) { }
void I_UpdateNoBlit(void) { }

void I_StartTic(void)
{
    MSG msg;
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) closing = true;
        TranslateMessage(&msg); DispatchMessage(&msg);
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
    if ((long long)outw * SCREENHEIGHT < (long long)outh * SCREENWIDTH) {
        left = 0; outw = r.right; outh = outw * SCREENHEIGHT / SCREENWIDTH; top = (r.bottom - outh) / 2;
    } else {
        top = 0; outh = r.bottom; outw = outh * SCREENWIDTH / SCREENHEIGHT; left = (r.right - outw) / 2;
    }
    PatBlt(dc, 0, 0, r.right, r.bottom, BLACKNESS);
    StretchDIBits(dc, left, top, outw, outh, 0, 0, SCREENWIDTH, SCREENHEIGHT,
                  rgb, &bmi, DIB_RGB_COLORS, SRCCOPY);
}

void I_ReadScreen(byte *scr) { memcpy(scr, screens[0], SCREENWIDTH * SCREENHEIGHT); }
void I_SetPalette(byte *pal) { memcpy(palette, pal, sizeof(palette)); }
