// ============================================================================
//  statusui.cpp — 状态面板
//
//  设计取舍（"简单点"）：
//    * 纯 Win32 子控件，程序化创建，不要 .rc / 不要 comctl32 / 不改 build.bat；
//    * 一个只读 EDIT 当状态区（等宽思路、按行拼），五个按钮 + 两个开关；
//    * 500ms 定时刷新，文本没变就不 SetWindowText（避免闪烁）；
//    * WM_CLOSE 只认"前台来源"：广播期间客户端会给不相关窗口群发 WM_CLOSE（实测），
//      不然面板会自己关掉。用户点 X 之前窗口一定已激活成前台，所以两边都不误伤。
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#include "statusui.h"
#include "config.h"
#include "facts.h"
#include "log.h"
#include "util.h"
#include "windowize.h"
#include "desktop.h"
#include "kbd.h"
#include "relay.h"
#include "bcast.h"

#include <stdio.h>
#include <stdlib.h>
#include <shellapi.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")

// ---------------- 控件 ID
#define IDC_STATUS   1001
#define IDC_FIXNOW   1002
#define IDC_PANIC    1003
#define IDC_DESK     1004
#define IDC_OPENLOG  1005
#define IDC_RELAY    1006
#define IDC_ACCEL    1007
#define IDC_SENS     1008
#define IDC_APPLY    1009
#define IDC_QUIT     1010
#define IDC_TIMER    1020

#define CLS_STATUSUI L"OSeasy-ToolStatusUi"

static HWND g_wnd = NULL, g_status = NULL, g_relayChk = NULL, g_accelChk = NULL, g_sensEdit = NULL;
static HWND g_btnPanic = NULL, g_btnDesk = NULL, g_btnFix = NULL, g_btnLog = NULL, g_btnQuit = NULL;
static HFONT g_uiFont = NULL, g_monoFont = NULL;
static wchar_t g_lastText[8192] = L"";

// ---------------------------------------------------------------- 文本
// 只有"广播窗口不在我们桌面上"（= B 方案没生效）时，才还有"课堂桌面"可切；
// 那时那个按钮变回"切到课堂桌面"，平时它是"看大画面（F11）"。
static BOOL NeedFallbackUi(void)
{
    return HasBroadcastWindow() && !BroadcastWindowOnOwnDesktop();
}

// 面板、--status、单实例回退弹框都用这一份文本，避免两处文案各写各的（之前就漂移过）
void StatusUiBuildText(wchar_t* s, int cch)
{
    long raw = 0, ignored = 0, relayed = 0, buttons = 0, hookSeen = 0, clamped = 0;
    int on = 0, absolute = 0;
    RelayGetStats(&raw, &ignored, &relayed, &buttons, &hookSeen, &on, &absolute);
    RelayGetDiag(&clamped);

    _snwprintf_s(s, cch, _TRUNCATE,
        L"【桌面】\r\n"
        L"  输入桌面   %ls   自己   %ls%ls\r\n"
        L"\r\n"
        L"【广播】\r\n"
        L"  广播窗口   %ls          改写为窗口广播   %d 次\r\n"
        L"  画面入口   %ls\r\n"
        L"  窗口化修正 %d 次        夺回桌面   %d 次\r\n"
        L"\r\n"
        L"【鼠标】\r\n"
        L"  客户端吞事件   %ls\r\n"
        L"  中继           %ls\r\n"
        L"  加速曲线       %ls   灵敏度 x%.2f\r\n"
        L"  位移 %ld 次 / 按键 %ld 次 / 夹边 %ld 次\r\n"
        L"  raw %ld 包（忽略注入 %ld）   钩子可见 %ld\r\n"
        L"\r\n"
        L"【键盘】\r\n"
        L"  %ls\r\n"
        L"\r\n"
        L"【热键】%ls\r\n"
        L"\r\n"
        L"日志: %ls",
        InputDesktopName(), OwnDesktopName(),
        DesktopIsOurs() ? L"" : L"   (正在夺回)",
        HasBroadcastWindow() ? L"有" : L"无", BcastFixCount(),
        NeedFallbackUi() ? L"F11 = 切到课堂桌面（广播在别的桌面上）"
                         : L"F11 = 看大画面（广播窗口铺满 / 还原）",
        WindowizeFixCount(), ReclaimCount(),
        RelaySwallowDetected() ? L"是（在吞）" : L"否",
        g_opt.mouseRelay ? (on ? L"接管中（直驱光标）"
                               : (absolute ? L"待命（绝对坐标设备，不接管）" : L"待命（等探测）"))
                         : L"已关闭",
        RelayGetAccel() ? L"开（甩动放大 2~4 倍）" : L"关（线性 1:1）",
        RelayGetSens(),
        relayed, buttons, clamped,
        raw, ignored, hookSeen,
        g_opt.keyboard ? (KbdAvailable() ? L"KbdFilter 释放中（每 400ms 下发一次）"
                                         : L"本机没有 KbdFilter 设备（本来也不锁）")
                       : L"已关闭",
        AppHotkeyDesc(),
        LogFilePath());
}

static void Refresh(void)
{
    if (!g_wnd) return;
    wchar_t buf[8192];
    StatusUiBuildText(buf, 8192);
    if (wcscmp(buf, g_lastText) != 0) {
        wcsncpy_s(g_lastText, 8192, buf, _TRUNCATE);
        SetWindowTextW(g_status, buf);
    }
    if (g_relayChk) SendMessageW(g_relayChk, BM_SETCHECK, g_opt.mouseRelay ? BST_CHECKED : BST_UNCHECKED, 0);
    if (g_accelChk) SendMessageW(g_accelChk, BM_SETCHECK, RelayGetAccel() ? BST_CHECKED : BST_UNCHECKED, 0);

    // 第一个按钮的语义跟真实状态走：平时是"看大画面（F11）"，B 失效时变成"切到课堂桌面"。
    BOOL fb = NeedFallbackUi();
    if (g_btnDesk) {
        SetWindowTextW(g_btnDesk, fb ? L"切到课堂桌面" : L"看大画面（F11）");
        ShowWindow(g_btnDesk, SW_SHOW);
    }

    const int y = 322, h = 30;
    UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
    if (fb) {
        // 切课堂桌面 / 窗口复位 / 紧急解封 / 日志 / 退出
        if (g_btnDesk)  SetWindowPos(g_btnDesk,  NULL, 12,  y, 146, h, flags);
        if (g_btnFix)   SetWindowPos(g_btnFix,   NULL, 166, y, 106, h, flags);
        if (g_btnPanic) SetWindowPos(g_btnPanic, NULL, 280, y, 106, h, flags);
        if (g_btnLog)   SetWindowPos(g_btnLog,   NULL, 394, y, 96,  h, flags);
        if (g_btnQuit)  SetWindowPos(g_btnQuit,  NULL, 498, y, 86,  h, flags);
    } else {
        // 正常情况五位：看大画面 / 窗口复位 / 紧急解封 / 日志 / 退出
        if (g_btnDesk)  SetWindowPos(g_btnDesk,  NULL, 12,  y, 120, h, flags);
        if (g_btnFix)   SetWindowPos(g_btnFix,   NULL, 140, y, 120, h, flags);
        if (g_btnPanic) SetWindowPos(g_btnPanic, NULL, 268, y, 106, h, flags);
        if (g_btnLog)   SetWindowPos(g_btnLog,   NULL, 382, y, 96,  h, flags);
        if (g_btnQuit)  SetWindowPos(g_btnQuit,  NULL, 486, y, 98,  h, flags);
    }
}

// ---------------------------------------------------------------- 动作
static void OpenLogDir(void)
{
    wchar_t dir[MAX_PATH];
    wcsncpy_s(dir, MAX_PATH, LogFilePath(), _TRUNCATE);
    wchar_t* s = wcsrchr(dir, L'\\');
    if (s) *s = 0;
    ShellExecuteW(NULL, L"open", dir, NULL, NULL, SW_SHOWNORMAL);
}

static void ApplySens(void)
{
    wchar_t t[32] = L"";
    GetWindowTextW(g_sensEdit, t, 32);
    double v = _wtof(t);
    if (v <= 0.0) { Log("ui: 灵敏度输入无效（%ls）", t); return; }
    RelaySetSens(v);
    RelaySaveSettings();
    Refresh();
}

// ---------------------------------------------------------------- 窗口过程
static LRESULT CALLBACK UiProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE: {
        HDC dc = GetDC(h);
        int dpi = GetDeviceCaps(dc, LOGPIXELSY);
        ReleaseDC(h, dc);
        int pt = 9;
        g_uiFont   = CreateFontW(-MulDiv(pt, dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        if (!g_uiFont) g_uiFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        g_monoFont = CreateFontW(-MulDiv(pt, dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");

        g_status = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                   WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                   12, 12, 574, 300, h, (HMENU)IDC_STATUS, NULL, NULL);
        struct { int id; const wchar_t* text; int x, y, cx, cy; } btns[] = {
            { IDC_DESK,    L"看大画面（F11）", 12,  322, 120, 30 },   // 第一位：看画面
            { IDC_FIXNOW,  L"窗口复位（F12）", 140, 322, 120, 30 },   // 第二位：复位
            { IDC_PANIC,   L"紧急解封",        268, 322, 106, 30 },   // 第三位：一把梭
            { IDC_OPENLOG, L"打开日志",        394, 322, 96,  30 },
            { IDC_QUIT,    L"退出程序",        498, 322, 86,  30 },
        };
        for (int i = 0; i < 5; i++) {
            HWND b = CreateWindowExW(0, L"BUTTON", btns[i].text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                     btns[i].x, btns[i].y, btns[i].cx, btns[i].cy, h,
                                     (HMENU)(LONG_PTR)btns[i].id, NULL, NULL);
            if (btns[i].id == IDC_PANIC)  g_btnPanic = b;
            if (btns[i].id == IDC_DESK)   g_btnDesk = b;
            if (btns[i].id == IDC_FIXNOW) g_btnFix = b;
            if (btns[i].id == IDC_OPENLOG) g_btnLog = b;
            if (btns[i].id == IDC_QUIT)   g_btnQuit = b;
        }

        g_relayChk = CreateWindowExW(0, L"BUTTON", L"鼠标中继（自动）",
                                     WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                     12, 366, 150, 24, h, (HMENU)IDC_RELAY, NULL, NULL);
        g_accelChk = CreateWindowExW(0, L"BUTTON", L"加速曲线",
                                     WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                     170, 366, 110, 24, h, (HMENU)IDC_ACCEL, NULL, NULL);
        CreateWindowExW(0, L"STATIC", L"灵敏度",
                        WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, 288, 366, 56, 24, h, NULL, NULL, NULL);
        g_sensEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                     WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_NUMBER | ES_CENTER,
                                     346, 366, 64, 24, h, (HMENU)IDC_SENS, NULL, NULL);
        CreateWindowExW(0, L"BUTTON", L"应用", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        416, 366, 82, 24, h, (HMENU)IDC_APPLY, NULL, NULL);

        // 字体：状态区用等宽，其余用界面字体
        SendMessageW(g_status, WM_SETFONT, (WPARAM)g_monoFont, TRUE);
        for (HWND c = GetWindow(h, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
            if (c != g_status) SendMessageW(c, WM_SETFONT, (WPARAM)g_uiFont, TRUE);

        wchar_t s[32];
        _snwprintf_s(s, 32, _TRUNCATE, L"%.2f", RelayGetSens());
        SetWindowTextW(g_sensEdit, s);

        SetTimer(h, IDC_TIMER, 500, NULL);
        Refresh();
        return 0;
    }
    case WM_TIMER:
        if (w == IDC_TIMER) Refresh();
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == IDC_FIXNOW) {
            int n = WindowizeNowManual();
            Log("ui: 窗口复位 -> 处理 %d 个窗口", n);
        } else if (id == IDC_PANIC) {
            // 和热键 Ctrl+Alt+F10 走同一段实现（main.cpp 的 AppPanicFix）：连按一下全给你办了
            Log("ui: 紧急解封 -> 窗口复位 + 键盘释放 + 夺回桌面 + 接管鼠标");
            AppPanicFix();
        } else if (id == IDC_DESK) {
            if (NeedFallbackUi()) {
                // B 方案没生效才有"课堂桌面"可切
                if (DesktopIsOurs()) {
                    if (SwitchToClassDesktop()) DesktopHoldReclaim(60000);   // 别 250ms 就把人拉回来
                } else {
                    ReclaimOwnDesktop();
                }
            } else {
                // 路线 C：看大画面 —— 广播窗口在工作区内铺满/还原，不切桌面
                if (WindowizeToggleMaximize() == 0)
                    MessageBoxW(h, L"当前没有广播窗口可放大。", L"OSeasy-Tool",
                                MB_ICONINFORMATION | MB_OK);
            }
        } else if (id == IDC_OPENLOG) {
            OpenLogDir();
        } else if (id == IDC_RELAY) {
            g_opt.mouseRelay = (SendMessageW(g_relayChk, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
            Log("ui: 鼠标中继 -> %s", g_opt.mouseRelay ? "开" : "关");
        } else if (id == IDC_ACCEL) {
            RelaySetAccel(SendMessageW(g_accelChk, BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0);
            RelaySaveSettings();
        } else if (id == IDC_APPLY) {
            ApplySens();
        } else if (id == IDC_QUIT) {
            AppRequestExit();          // 面板也能退出（托盘图标找不到时的救命入口）
            return 0;
        }
        Refresh();
        return 0;
    }
    case WM_CLOSE:
        // 只认前台来源（客户端广播期间会给不相关窗口群发 WM_CLOSE，实测能刷到 0.5 秒一次）
        if (GetForegroundWindow() == h) {
            DestroyWindow(h);
        } else {
            static DWORD last = 0;
            DWORD now = GetTickCount();
            if (now - last > 10000) { Log("ui: 忽略一条非前台来源的 WM_CLOSE（广播骚扰）"); last = now; }
        }
        return 0;
    case WM_DESTROY:
        KillTimer(h, IDC_TIMER);
        g_wnd = g_status = g_relayChk = g_accelChk = g_sensEdit = NULL;
        g_lastText[0] = 0;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ---------------------------------------------------------------- 对外
void StatusUiShow(void)
{
    if (g_wnd) {
        if (IsIconic(g_wnd)) ShowWindow(g_wnd, SW_RESTORE);
        // 强制"可见 + 前台"：实测有过窗口存在但 WS_VISIBLE 被清掉的情况（面板就打不开了）
        SetWindowPos(g_wnd, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetForegroundWindow(g_wnd);
        Refresh();
        return;
    }
    WNDCLASSEXW wc; memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = UiProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = CLS_STATUSUI;
    RegisterClassExW(&wc);

    RECT r = { 0, 0, 600, 402 };
    AdjustWindowRectEx(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0);
    int w = r.right - r.left, h = r.bottom - r.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 4;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    g_wnd = CreateWindowExW(0, CLS_STATUSUI, L"OSeasy-Tool 状态",
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                            x, y, w, h, NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!g_wnd) { Log("ui: 状态面板创建失败 err=%lu", GetLastError()); return; }
    ShowWindow(g_wnd, SW_SHOW);
    SetForegroundWindow(g_wnd);
}

void StatusUiClose(void)
{
    if (g_wnd) DestroyWindow(g_wnd);
    if (g_uiFont && g_uiFont != (HFONT)GetStockObject(DEFAULT_GUI_FONT)) DeleteObject(g_uiFont);
    if (g_monoFont) DeleteObject(g_monoFont);
    g_uiFont = g_monoFont = NULL;
}
