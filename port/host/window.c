#include "window.h"
#include "hle_hw.h"
#include "host.h"
#include "input.h"

#include <stdio.h>

static HWND g_hwnd;

static LRESULT CALLBACK wndproc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_KEYDOWN:
        /* Esc quits, except in the operator menu, where it goes back (input.c) or cancels a prompt. */
        if (wp == VK_ESCAPE && !input_capturing() && !input_operator_menu_open())
            PostMessageA(w, WM_CLOSE, 0, 0);
        break;
    case WM_CHAR:
        input_push_char((int)wp);
        break;
    case WM_CLOSE:
        hy_log("window closed");
        window_quit();
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(NULL);
            return TRUE;
        }
        break;
    }
    return DefWindowProcA(w, msg, wp, lp);
}

void window_create(int width, int height)
{
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hIcon = LoadIconA(wc.hInstance, MAKEINTRESOURCEA(1)); /* hydro.rc */
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "HydroThunder";
    RegisterClassA(&wc);

    RECT r = {0, 0, width, height};
    DWORD style = WS_OVERLAPPEDWINDOW & ~(WS_MAXIMIZEBOX | WS_THICKFRAME);
    AdjustWindowRect(&r, style, FALSE);
    char title[32] = "Hydro Thunder";
    if (g_cfg.net_unit)
        snprintf(title, sizeof title, "Hydro Thunder (unit %d)", g_cfg.net_unit);
    g_hwnd = CreateWindowA("HydroThunder", title, style | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    if (!g_hwnd)
        hy_fatal("CreateWindow failed (%lu)", GetLastError());
    window_pump();
}

void window_quit(void)
{
    hw_cmos_flush_volume();
    ExitProcess(0);
}

HWND window_get(void)
{
    return g_hwnd;
}

void window_pump(void)
{
    MSG m;
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
}
