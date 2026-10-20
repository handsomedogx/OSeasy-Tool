// ============================================================================
//  mirror.cpp — 镜像窗口：把自己桌面上的一小块"电视"指向老师的广播画面
//
//  为什么可行（实测）：广播窗口即使不在"当前桌面"上，PrintWindow(PW_RENDERFULLCONTENT)
//  仍然能取到内容 —— 用带动图的假广播窗口实测两帧间 99.96% 采样发生变化；真实广播窗口
//  在桌面非活动时抓到的画面 98% 非黑。所以"夺回自己桌面 + 抓图显示"这条路是通的。
//
//  结构：
//    * UI 线程      ：在我们自己的桌面上创建镜像窗口，负责把帧缓冲贴到窗口上
//    * 抓帧线程     ：找到广播窗口(任意桌面) -> 必要时 SetThreadDesktop 切过去 -> PrintWindow
//                     （该线程不创建任何窗口，所以可以反复切换桌面）
//  两个线程通过 DIB + 临界区 + PostMessage 通信。
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#include "mirror.h"
#include "config.h"
#include "facts.h"
#include "log.h"
#include "util.h"
#include <stdio.h>

#define WM_MIRROR_FRAME (WM_APP + 11)

static HWND   g_wnd = NULL;
static HANDLE g_uiThread = NULL, g_capThread = NULL;
static volatile LONG g_stop = 0, g_frameSeq = 0;
static volatile LONG g_userHidden = 0;    // 用户主动隐藏后，抓帧循环不再自动弹回来

// 帧缓冲
static CRITICAL_SECTION g_cs;
static BOOL   g_csInit = FALSE;
static void*  g_bits = NULL;
static HDC    g_memDc = NULL;
static HBITMAP g_bmp = NULL;
static int    g_fw = 0, g_fh = 0;
static DWORD  g_lastFrameTick = 0;

// ---------------------------------------------------------------- UI
static void Paint(HDC dc, const RECT* rc)
{
    EnterCriticalSection(&g_cs);
    if (g_bits && g_fw > 0 && g_fh > 0 && g_memDc) {
        int cw = rc->right - rc->left, chh = rc->bottom - rc->top;
        double sx = (double)cw / g_fw, sy = (double)chh / g_fh;
        double s = sx < sy ? sx : sy;                     // 等比缩放
        int dw = (int)(g_fw * s), dh = (int)(g_fh * s);
        int dx = rc->left + (cw - dw) / 2, dy = rc->top + (chh - dh) / 2;
        SetStretchBltMode(dc, HALFTONE);
        StretchBlt(dc, dx, dy, dw, dh, g_memDc, 0, 0, g_fw, g_fh, SRCCOPY);
    } else {
        RECT all = *rc;
        FillRect(dc, &all, (HBRUSH)GetStockObject(BLACK_BRUSH));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(200, 200, 200));
        const wchar_t* msg = L"等待广播画面…（老师开始屏幕广播后这里会自动出现）";
        DrawTextW(dc, msg, -1, &all, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    LeaveCriticalSection(&g_cs);
}

static LRESULT CALLBACK MirrorProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        Paint(dc, &rc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MIRROR_FRAME:
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_SIZE:
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_CLOSE:
        // 广播期间客户端会反复给不相关窗口发 WM_CLOSE（实测能把镜像关到 0.5 秒闪一次），
        // 但用户点 X 之前窗口一定会先被激活成前台窗口 —— 用这个区分，两边都不误伤。
        if (GetForegroundWindow() == h) {
            ShowWindow(h, SW_HIDE);
            InterlockedExchange(&g_userHidden, 1);
            Log("mirror: 用户点了 X -> 已隐藏（托盘菜单可再显示；关闭期间不会再自动弹出）");
        } else {
            static DWORD lastLog2 = 0;
            DWORD now2 = GetTickCount();
            if (now2 - lastLog2 > 10000) {
                Log("mirror: 忽略一条非前台来源的 WM_CLOSE（客户端广播期间的骚扰）");
                lastLog2 = now2;
            }
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static DWORD WINAPI UiThreadProc(LPVOID)
{
    WNDCLASSEXW wc; memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MirrorProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"OeWinFix2Mirror";
    RegisterClassExW(&wc);

    int w = 960, h = 540;
    int sx = GetSystemMetrics(SM_CXSCREEN) - w - 40;
    int sy = GetSystemMetrics(SM_CYSCREEN) - h - 80;
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;
    g_wnd = CreateWindowExW(WS_EX_TOPMOST, L"OeWinFix2Mirror",
                            L"广播画面（镜像） — 可拖动/缩放；点一下窗口再按 X 隐藏，退出用托盘菜单",
                            WS_OVERLAPPEDWINDOW, sx, sy, w, h,
                            NULL, NULL, wc.hInstance, NULL);
    if (!g_wnd) { Log("mirror: 窗口创建失败 err=%lu", GetLastError()); return 0; }
    ShowWindow(g_wnd, SW_SHOWNOACTIVATE);

    MSG msg;
    while (!g_stop && GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        // 3 秒没帧 -> 自动隐藏（广播结束）
        if (g_wnd && IsWindowVisible(g_wnd) && g_lastFrameTick &&
            GetTickCount() - g_lastFrameTick > 3000) {
            ShowWindow(g_wnd, SW_HIDE);
            g_lastFrameTick = 0;
            Log("mirror: 3 秒没有新帧 -> 自动隐藏（广播可能已结束）");
        }
    }
    if (g_wnd) { DestroyWindow(g_wnd); g_wnd = NULL; }
    return 0;
}

// ---------------------------------------------------------------- 抓帧
typedef struct { HWND hwnd; wchar_t desk[128]; } TARGET;

static BOOL CALLBACK FindRender(HWND h, LPARAM lp)
{
    if (!IsWindowVisible(h)) return TRUE;
    wchar_t cls[128] = {0};
    GetClassNameW(h, cls, 128);
    if (_wcsicmp(cls, BCAST_WINDOW_CLASS) == 0) { *(HWND*)lp = h; return FALSE; }
    return TRUE;
}

static BOOL FindTarget(TARGET* t)
{
    t->hwnd = NULL; t->desk[0] = 0;
    EnumDesktopWindows(NULL, FindRender, (LPARAM)&t->hwnd);
    if (t->hwnd) { wcsncpy_s(t->desk, 128, OwnDesktopName(), _TRUNCATE); return TRUE; }

    const wchar_t* desks[2] = { TEACH_DESKTOP_NAME, TEACH_DESKTOP_NAME2 };
    for (int i = 0; i < 2; i++) {
        HDESK d = OpenDeskByName(desks[i], DESKTOP_READOBJECTS | DESKTOP_ENUMERATE);
        if (!d) continue;
        EnumDesktopWindows(d, FindRender, (LPARAM)&t->hwnd);
        CloseDesktop(d);
        if (t->hwnd) { wcsncpy_s(t->desk, 128, desks[i], _TRUNCATE); return TRUE; }
    }
    return FALSE;
}

static BOOL EnsureFrame(int w, int h)
{
    EnterCriticalSection(&g_cs);
    if (g_bits && g_fw == w && g_fh == h) { LeaveCriticalSection(&g_cs); return TRUE; }
    if (g_bmp) { DeleteObject(g_bmp); g_bmp = NULL; }
    if (g_memDc) { DeleteDC(g_memDc); g_memDc = NULL; }
    g_bits = NULL; g_fw = g_fh = 0;

    HDC screenDc = GetDC(NULL);
    BITMAPINFO bi; memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;      // 自上而下
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = NULL;
    HBITMAP bmp = CreateDIBSection(screenDc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    BOOL ok = FALSE;
    if (bmp && bits) {
        g_memDc = CreateCompatibleDC(screenDc);
        SelectObject(g_memDc, bmp);
        g_bmp = bmp; g_bits = bits; g_fw = w; g_fh = h;
        ok = TRUE;
    }
    ReleaseDC(NULL, screenDc);
    LeaveCriticalSection(&g_cs);
    return ok;
}

static DWORD WINAPI CaptureProc(LPVOID)
{
    wchar_t attached[128] = L"";
    Log("mirror: capture thread on (%d fps)", g_opt.mirrorFps);
    int fps = g_opt.mirrorFps > 0 ? g_opt.mirrorFps : 10;
    int idleRounds = 0;
    do {
        TARGET t;
        if (!FindTarget(&t)) {
            if (++idleRounds == 5) Log("mirror: 没找到广播窗口（等待老师开始广播）");
            Sleep(500);
            continue;
        }
        idleRounds = 0;

        // 窗口广播（窗口就在我们自己桌面上）时不需要镜像 —— 否则同一幅画面会出现两次。
        // 只有"广播窗口在别的桌面上"（=全屏广播被夺回桌面后）才显示镜像。
        if (_wcsicmp(t.desk, OwnDesktopName()) == 0) {
            if (g_wnd && IsWindowVisible(g_wnd) && !g_userHidden) {
                ShowWindow(g_wnd, SW_HIDE);
                Log("mirror: 广播窗口已在我们自己的桌面上 -> 隐藏镜像（不需要重复显示）");
            }
            Sleep(500);
            continue;
        }

        // 目标不在我们的桌面上 -> 本线程切过去（本线程无窗口/钩子，可反复切换）
        if (_wcsicmp(t.desk, attached) != 0) {
            HDESK d = OpenDeskByName(t.desk, DESKTOP_READOBJECTS | DESKTOP_ENUMERATE);
            if (d) {
                if (SetThreadDesktop(d)) {
                    wcsncpy_s(attached, 128, t.desk, _TRUNCATE);
                    Log("mirror: 抓帧线程已附着到桌面 '%ls'", t.desk);
                } else {
                    Log("mirror: SetThreadDesktop('%ls') failed err=%lu（继续用当前桌面尝试）", t.desk, GetLastError());
                }
                CloseDesktop(d);
            }
        }

        RECT rc; GetWindowRect(t.hwnd, &rc);
        int w = rc.right - rc.left, h = rc.bottom - rc.top;
        if (w > 0 && h > 0 && EnsureFrame(w, h)) {
            EnterCriticalSection(&g_cs);
            BOOL ok = PrintWindow(t.hwnd, g_memDc, 2 /*PW_RENDERFULLCONTENT*/);
            LeaveCriticalSection(&g_cs);
            if (ok) {
                InterlockedIncrement(&g_frameSeq);
                g_lastFrameTick = GetTickCount();
                if (g_wnd) {
                    if (!IsWindowVisible(g_wnd) && !g_userHidden) {
                        ShowWindow(g_wnd, SW_SHOWNOACTIVATE);
                        Log("mirror: 检测到广播画面 -> 显示镜像窗口");
                    }
                    PostMessageW(g_wnd, WM_MIRROR_FRAME, 0, 0);
                }
            }
        }
        Sleep(1000 / fps);
    } while (!g_stop);
    Log("mirror: capture thread exit (%ld 帧)", (long)g_frameSeq);
    return 0;
}

// ---------------------------------------------------------------- 对外
void MirrorStart(void)
{
    if (!g_csInit) { InitializeCriticalSection(&g_cs); g_csInit = TRUE; }
    InterlockedExchange(&g_stop, 0);
    g_uiThread  = CreateThread(NULL, 0, UiThreadProc, NULL, 0, NULL);
    g_capThread = CreateThread(NULL, 0, CaptureProc, NULL, 0, NULL);
    Log("mirror: starting（镜像窗口 + 抓帧线程）");
}

void MirrorStop(void)
{
    InterlockedExchange(&g_stop, 1);
    if (g_wnd) PostMessageW(g_wnd, WM_QUIT, 0, 0);
    HANDLE hs[2] = { g_uiThread, g_capThread };
    for (int i = 0; i < 2; i++) {
        if (!hs[i]) continue;
        WaitForSingleObject(hs[i], 2000);
        CloseHandle(hs[i]);
    }
    g_uiThread = g_capThread = NULL;
    EnterCriticalSection(&g_cs);
    if (g_memDc) { DeleteDC(g_memDc); g_memDc = NULL; }
    if (g_bmp) { DeleteObject(g_bmp); g_bmp = NULL; }
    g_bits = NULL; g_fw = g_fh = 0;
    LeaveCriticalSection(&g_cs);
    Log("mirror: stopped");
}

HWND MirrorWindow(void) { return g_wnd; }

void MirrorSetUserHidden(int hidden)
{
    InterlockedExchange(&g_userHidden, hidden ? 1 : 0);
    if (!g_wnd) return;
    ShowWindow(g_wnd, hidden ? SW_HIDE : SW_SHOWNOACTIVATE);
    if (!hidden) SetForegroundWindow(g_wnd);
    Log("mirror: 用户%s镜像窗口", hidden ? "隐藏了" : "显示了");
}

