// ============================================================================
//  FakeBox.cpp — 测试替身：复刻"广播中的客户端"对鼠标做的事
//
//  它做四件事（都有实测依据）：
//    1. 注册窗口类 MultiRenderWindowClass，建全屏置顶窗口（= 广播窗口），可 --desk 指定桌面
//    2. 加载**真实** StudentLogic.dll，用它自己的钩子过程(RVA 0x80190)装 WH_MOUSE_LL
//       —— 只放行 dwExtraInfo==0x12452，其余真实鼠标事件 return 1 吞掉
//    3. ShowCursor(FALSE) 隐藏光标（学生端也这样）
//    4. 定时自注入点击/移动，打印"被吞还是放行"，用来自检
//
//  用法:
//    FakeBox.exe lock                      基本复刻（全屏窗口 + 真钩子）
//    FakeBox.exe lock --desk NetTeachingDesktop   在指定桌面上建窗口（复刻教学桌面）
//    FakeBox.exe lock --live               窗口自绘动图（肉眼确认窗口化/中继时画面还在动）
//    FakeBox.exe lock --inject 3000        每 3 秒自注入一次点击（0 = 关）
//    FakeBox.exe lock --fight 1500         每 1.5 秒重新强占全屏（验证窗口化守护抗刷新）
//  ESC 退出；P 打印窗口样式/钩子字节。
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "kernel32.lib")
#pragma comment(lib, "gdi32.lib")

#define HOOK_PROC_RVA 0x80190
#define MAGIC_EXTRA   0x12452
#define APP_DIR       L"C:\\Program Files (x86)\\Os-Easy\\os-easy multicast teaching system"

static HHOOK   g_hook = NULL;
static HWND    g_wnd = NULL;
static HMODULE g_sl = NULL;
static BOOL    g_live = FALSE;
static UINT    g_fightMs = 0, g_injectMs = 3000;
static wchar_t g_desk[128] = L"";
static volatile LONG g_clickSent = 0, g_clickRecv = 0, g_paintTick = 0;

static LRESULT CALLBACK FakeHookProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == 0) {
        MSLLHOOKSTRUCT* p = (MSLLHOOKSTRUCT*)lParam;
        if (p->dwExtraInfo != MAGIC_EXTRA) return 1;      // 复刻客户端：吞掉真实事件
    }
    return CallNextHookEx(g_hook, nCode, wParam, lParam);
}

static void PaintLive(HWND h)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT rc; GetClientRect(h, &rc);
    LONG t = InterlockedIncrement(&g_paintTick);
    HBRUSH bg = CreateSolidBrush(RGB((t * 7) % 200 + 30, (t * 11) % 200 + 30, (t * 17) % 200 + 30));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    HBRUSH bar = CreateSolidBrush(RGB(255, 255, 0));
    int barW = 240, x = (int)((t * 23) % (rc.right + barW)) - barW;
    RECT rb = { x, 0, x + barW, rc.bottom };
    FillRect(dc, &rb, bar);
    DeleteObject(bar);
    wchar_t txt[128];
    _snwprintf_s(txt, 128, _TRUNCATE, L"FAKE BROADCAST  frame=%ld  %lu ms", t, GetTickCount());
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    TextOutW(dc, 40, 40, txt, (int)wcslen(txt));
    EndPaint(h, &ps);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_LBUTTONDOWN:
        InterlockedIncrement(&g_clickRecv);
        return 0;
    case WM_PAINT:
        if (g_live) { PaintLive(h); return 0; }
        break;
    case WM_ERASEBKGND:
        if (g_live) return 1;
        break;
    case WM_TIMER:
        if (w == 1) {
            int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
            SetWindowLongW(h, GWL_EXSTYLE, GetWindowLongW(h, GWL_EXSTYLE) | WS_EX_TOPMOST);
            SetWindowPos(h, HWND_TOPMOST, 0, 0, sw, sh, SWP_SHOWWINDOW | SWP_NOACTIVATE);
            SetForegroundWindow(h);
            printf("[FakeBox] re-assert fullscreen (every %u ms)\n", g_fightMs);
            fflush(stdout);
        } else if (w == 2) {
            int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
            SetCursorPos(sw / 2, sh / 2);
            InterlockedIncrement(&g_clickSent);
            mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
            mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
            printf("[FakeBox] clicks sent=%d received=%d -> %s\n", (int)g_clickSent, (int)g_clickRecv,
                   g_clickRecv == 0 ? "MOUSE BLOCKED (hook swallowing)" : "MOUSE PASSING");
            fflush(stdout);
        } else if (w == 3) {
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) { DestroyWindow(h); return 0; }
        if (w == 'P') {
            RECT rc; GetWindowRect(h, &rc);
            printf("[FakeBox] hwnd=0x%08X style=0x%08X ex=0x%08X rect=(%d,%d,%d,%d)\n",
                   (unsigned)(ULONG_PTR)h, (unsigned)GetWindowLongW(h, GWL_STYLE),
                   (unsigned)GetWindowLongW(h, GWL_EXSTYLE), rc.left, rc.top, rc.right, rc.bottom);
            if (g_sl) {
                BYTE* p = (BYTE*)g_sl + HOOK_PROC_RVA;
                __try {
                    printf("[FakeBox] hook bytes @StudentLogic+0x%X : %02X %02X %02X %02X %02X  swallow@+0x801AC: %02X %02X %02X %02X\n",
                           HOOK_PROC_RVA, p[0], p[1], p[2], p[3], p[4],
                           *(BYTE*)((BYTE*)g_sl + 0x801AC), *(BYTE*)((BYTE*)g_sl + 0x801AD),
                           *(BYTE*)((BYTE*)g_sl + 0x801AE), *(BYTE*)((BYTE*)g_sl + 0x801AF));
                } __except (EXCEPTION_EXECUTE_HANDLER) { printf("[FakeBox] hook bytes unreadable\n"); }
            }
            fflush(stdout);
            return 0;
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static DWORD WINAPI Worker(LPVOID)
{
    if (g_desk[0]) {
        HDESK d = OpenDesktopW(g_desk, 0, FALSE, DESKTOP_CREATEWINDOW | DESKTOP_READOBJECTS |
                              DESKTOP_ENUMERATE | DESKTOP_SWITCHDESKTOP | DESKTOP_WRITEOBJECTS);
        if (!d) { printf("[FakeBox] OpenDesktop('%ls') failed err=%lu\n", g_desk, GetLastError()); return 1; }
        if (!SetThreadDesktop(d)) {
            printf("[FakeBox] SetThreadDesktop('%ls') failed err=%lu\n", g_desk, GetLastError());
            return 1;
        }
        printf("[FakeBox] thread attached to desktop '%ls'\n", g_desk);
    }

    g_sl = LoadLibraryW(APP_DIR L"\\StudentLogic.dll");
    if (g_sl) {
        HOOKPROC proc = (HOOKPROC)((BYTE*)g_sl + HOOK_PROC_RVA);
        g_hook = SetWindowsHookExW(WH_MOUSE_LL, proc, g_sl, 0);
        printf("[FakeBox] real StudentLogic.dll @0x%08X, WH_MOUSE_LL hook=0x%08X (%s)\n",
               (unsigned)(ULONG_PTR)g_sl, (unsigned)(ULONG_PTR)g_hook,
               g_hook ? "installed" : "FAILED (err=");
        if (!g_hook) printf("[FakeBox]   SetWindowsHookEx err=%lu\n", GetLastError());
    } else {
        g_hook = SetWindowsHookExW(WH_MOUSE_LL, FakeHookProc, NULL, 0);
        printf("[FakeBox] StudentLogic.dll 加载失败(err=%lu) -> 用内置等价钩子 hook=0x%08X\n",
               GetLastError(), (unsigned)(ULONG_PTR)g_hook);
    }
    if (!g_hook) printf("[FakeBox] 钩子装不上（非输入桌面上装全局鼠标钩子会 err=5）\n");

    WNDCLASSEXW wc; memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"MultiRenderWindowClass";
    RegisterClassExW(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    g_wnd = CreateWindowExW(WS_EX_TOPMOST, L"MultiRenderWindowClass", L"FAKE fullscreen broadcast",
                            WS_POPUP | WS_VISIBLE, 0, 0, sw, sh, NULL, NULL, wc.hInstance, NULL);
    if (!g_wnd) { printf("[FakeBox] CreateWindow failed err=%lu\n", GetLastError()); return 1; }
    printf("[FakeBox] fullscreen window created (class=MultiRenderWindowClass%s%ls) ESC=quit P=dump\n",
           g_desk[0] ? ", desktop=" : "", g_desk[0] ? g_desk : L"");
    while (ShowCursor(FALSE) >= 0) { }

    if (g_fightMs) SetTimer(g_wnd, 1, g_fightMs, NULL);
    if (g_injectMs) SetTimer(g_wnd, 2, g_injectMs, NULL);
    if (g_live) { SetTimer(g_wnd, 3, 120, NULL); InvalidateRect(g_wnd, NULL, TRUE); }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_hook) UnhookWindowsHookEx(g_hook);
    return 0;
}

int wmain(int argc, wchar_t** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    for (int i = 1; i < argc; i++) {
        if (_wcsicmp(argv[i], L"--desk") == 0 && i + 1 < argc) wcsncpy_s(g_desk, 128, argv[++i], _TRUNCATE);
        else if (_wcsicmp(argv[i], L"--live") == 0) g_live = TRUE;
        else if (_wcsicmp(argv[i], L"--inject") == 0 && i + 1 < argc) g_injectMs = (UINT)_wtoi(argv[++i]);
        else if (_wcsicmp(argv[i], L"--fight") == 0 && i + 1 < argc) g_fightMs = (UINT)_wtoi(argv[++i]);
    }
    SetCurrentDirectoryW(APP_DIR);
    HANDLE t = CreateThread(NULL, 0, Worker, NULL, 0, NULL);
    if (!t) return 1;
    WaitForSingleObject(t, INFINITE);
    return 0;
}
