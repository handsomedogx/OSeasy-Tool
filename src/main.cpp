// ============================================================================
//  OeWinFix2 — 只有两件事
//    ① 全屏广播窗口 -> 窗口化
//    ② 保证键盘鼠标正常使用（+ 镜像窗口，让你留在自己桌面也能看到老师画面）
//
//  重写说明：所有机制都来自旧项目 OeWinFix 的**实测结论**，见 src/facts.h 与 docs/evidence.md。
//  与旧版的区别：不再有内存补丁（主客户端被驱动保护，打不进去）、不再有学生端结束/挂起、
//  不再有 MMPC 服务控制、不再有十来个诊断开关与两层 CLI 解析。
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include "config.h"
#include "facts.h"
#include "log.h"
#include "util.h"
#include "windowize.h"
#include "desktop.h"
#include "kbd.h"
#include "relay.h"
#include "bcast.h"
#include "mirror.h"
#include "statusui.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "kernel32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")

OPTIONS g_opt = {
    1, 1, 1, 1, -1, 1, 1, 1,   // windowize/desktopReclaim/mirror/mouseRelay(默认开:探测驱动)
                               // /mouseAccel(-1 = 未指定, 启动时读 HKCU; 默认关=线性)/bcastFix/keyboard/tray
    200, 250, 400, 10,         // windowInterval/desktopInterval/kbdInterval/mirrorFps
    0, 0, 0, 0,                // winW/winH/winX/winY  (0 = 工作区 80% 居中)
    1,                         // noResize: **默认 1 = 守护不主动改尺寸位置**（手动窗口化仍会改）
    0, 0, 0, 0, 0, 0, 1        // verbose/once/relayForce/showStatus/windowizeNow/quit/showPanel(默认弹面板)
};

#define WM_TRAY (WM_APP + 21)
#define IDM_SHOW_MIRROR 3001
#define IDM_WINDOWIZE   3002
#define IDM_TOGGLE_DESK 3003
#define IDM_RELAY_TOG   3004
#define IDM_SHOW_STATUS 3005
#define IDM_OPEN_LOG    3006
#define IDM_EXIT        3007
#define IDM_FIXONCE     3008
#define IDM_BCAST_FIX   3009
#define IDM_RELAY_ACCEL 3010
#define HOTKEY_FIX      0xB001
#define HOTKEY_PANIC    0xB002

static NOTIFYICONDATAW g_nid;
static BOOL g_trayAdded = FALSE;
static BOOL g_realExit = FALSE;        // 只有托盘菜单"退出程序"才置位
static HWND g_hidden = NULL;          // 只用来收托盘消息/热键的隐藏窗口
static wchar_t g_logPath[MAX_PATH] = L"";
static double g_sensOpt = -1.0;        // --sens（<0 = 未指定）

// 第二个实例 -> 已有实例：请它弹面板 / 退出（不依赖托盘图标，也不靠 MessageBox 干等）
#define OWF2_MSG_SHOWUI  (WM_APP + 31)
#define OWF2_MSG_QUIT    (WM_APP + 32)
static UINT g_wmTaskbarCreated = 0;

// 判断"事情真的发生了"用观察到的窗口状态，不用命名事件握手
// （实测：命名事件 ACK 偶发收不到，消息其实已经生效 -> 会退回弹框，观感就是"又只弹了个框"）
static BOOL WaitWindow(const wchar_t* cls, DWORD ms, BOOL wantVisible)
{
    DWORD t0 = GetTickCount();
    for (;;) {
        HWND h = FindWindowW(cls, NULL);
        if (h && (!wantVisible || IsWindowVisible(h))) return TRUE;
        if (GetTickCount() - t0 >= ms) return FALSE;
        Sleep(50);
    }
}

static BOOL AskRunningShowPanel(DWORD ms)
{
    HWND h = FindWindowW(L"OeWinFix2Status", NULL);
    if (!h) return FALSE;
    PostMessageW(h, OWF2_MSG_SHOWUI, 0, 0);
    return WaitWindow(L"OeWinFix2StatusUi", ms, TRUE);      // 面板真的出现了才算成功
}

static BOOL AskRunningQuit(DWORD ms)
{
    HWND h = FindWindowW(L"OeWinFix2Status", NULL);
    if (!h) return FALSE;
    PostMessageW(h, OWF2_MSG_QUIT, 0, 0);
    DWORD t0 = GetTickCount();
    for (;;) {                                              // 等它的宿主窗口消失
        if (!FindWindowW(L"OeWinFix2Status", NULL)) return TRUE;
        if (GetTickCount() - t0 >= ms) return FALSE;
        Sleep(50);
    }
}

// 面板上的"退出程序"走这里：走正常退出路径（停线程、卸托盘），不是 TerminateProcess
void AppRequestExit(void)
{
    g_realExit = TRUE;
    if (g_hidden) PostMessageW(g_hidden, WM_CLOSE, 0, 0);
}

// ---------------------------------------------------------------- 状态文本
// 状态文字只有一份，在 statusui.cpp（面板和 --status/弹框共用），这里不再重复拼一遍
static void ShowStatusBox(void)
{
    wchar_t s[2048];
    StatusUiBuildText(s, 2048);
    MessageBoxW(NULL, s, L"OeWinFix2 状态", MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
}

// ---------------------------------------------------------------- 托盘
static void TrayAdd(HWND h)
{
    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = h;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_INFO;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wcsncpy_s(g_nid.szTip, 128, L"OeWinFix2：广播窗口化 + 输入恢复（双击看状态面板）", _TRUNCATE);
    // 第一次启动给一个气泡：否则用户会以为"没有托盘程序"（图标可能被 Windows 收进"隐藏的图标"里）
    wcsncpy_s(g_nid.szInfoTitle, 64, L"OeWinFix2 已启动", _TRUNCATE);
    wcsncpy_s(g_nid.szInfo, 256,
              L"图标在通知区（可能收在“隐藏的图标”里）。双击打开状态面板，面板里也有“退出程序”。", _TRUNCATE);
    g_nid.dwInfoFlags = NIIF_INFO;
    for (int i = 0; i < 3 && !g_trayAdded; i++) {      // 通知区没准备好时重试几次
        g_trayAdded = Shell_NotifyIconW(NIM_ADD, &g_nid) ? TRUE : FALSE;
        if (!g_trayAdded) Sleep(500);
    }
    if (g_trayAdded) Log("tray: 图标已添加（右键菜单；双击看状态面板）");
    else             Log("tray: 图标添加失败（Shell_NotifyIcon 返回 FALSE）—— 别慌：面板仍然可用");
}

static void TrayRemove(void)
{
    if (g_trayAdded) { Shell_NotifyIconW(NIM_DELETE, &g_nid); g_trayAdded = FALSE; }
}

static void TrayMenu(HWND h)
{
    POINT pt; GetCursorPos(&pt);
    HMENU m = CreatePopupMenu();
    // B 方案生效时（广播窗口就在我们桌面上），"镜像/切桌面"没有意义 -> 置灰并说明
    BOOL fb = HasBroadcastWindow() && !BroadcastWindowOnOwnDesktop();
    AppendMenuW(m, MF_STRING, IDM_SHOW_STATUS, L"显示状态面板");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_FIXONCE, L"窗口复位：摆成工作区 80% 居中 (Ctrl+Alt+F9)");
    AppendMenuW(m, MF_STRING, IDM_TOGGLE_DESK,
                fb ? L"切到课堂桌面 / 回自己桌面 (Ctrl+Alt+F8)"
                   : L"看大画面：广播窗口最大化 / 还原 (Ctrl+Alt+F8)");
    AppendMenuW(m, MF_STRING | (fb ? 0 : MF_GRAYED), IDM_SHOW_MIRROR,
                fb ? L"显示/隐藏镜像窗口"
                   : L"镜像窗口 —— 广播窗口就在你桌面上，不需要镜像");
    AppendMenuW(m, MF_STRING | (g_opt.mouseRelay ? MF_CHECKED : 0), IDM_RELAY_TOG, L"鼠标中继（自动：探测到被吞才接管）");
    AppendMenuW(m, MF_STRING | (RelayGetAccel() ? MF_CHECKED : 0), IDM_RELAY_ACCEL, L"鼠标加速曲线（关 = 线性，灵敏度低但可预测）");
    AppendMenuW(m, MF_STRING | (g_opt.bcastFix ? MF_CHECKED : 0), IDM_BCAST_FIX, L"全屏广播改写为窗口广播");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_OPEN_LOG, L"打开日志目录");
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"退出程序");
    SetForegroundWindow(h);
    int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, NULL);
    DestroyMenu(m);
    switch (cmd) {
    case IDM_SHOW_STATUS: StatusUiShow(); break;
    case IDM_FIXONCE: {
        int n = WindowizeNowManual();
        wchar_t b[128];
        _snwprintf_s(b, 128, _TRUNCATE, L"窗口化：处理了 %d 个窗口", n);
        Log("tray: 窗口复位 -> %d", n);
        if (n == 0) MessageBoxW(NULL, L"当前没有发现广播窗口（MultiRenderWindowClass）。", L"OeWinFix2", MB_ICONINFORMATION | MB_OK);
        break;
    }
    case IDM_TOGGLE_DESK:
        // B 方案没生效 -> 还有"课堂桌面"可切；生效了 -> 这个位置就是"看大画面"
        if (HasBroadcastWindow() && !BroadcastWindowOnOwnDesktop()) {
            if (DesktopIsOurs()) { if (SwitchToClassDesktop()) DesktopHoldReclaim(60000); }
            else ReclaimOwnDesktop();
        } else if (WindowizeToggleMaximize() == 0) {
            MessageBoxW(NULL, L"当前没有广播窗口可放大。", L"OeWinFix2", MB_ICONINFORMATION | MB_OK);
        }
        break;
    case IDM_SHOW_MIRROR: {
        HWND w = MirrorWindow();
        if (!w) { g_opt.mirror = 1; MirrorStart(); }
        else MirrorSetUserHidden(IsWindowVisible(w) ? 1 : 0);
        break;
    }
    case IDM_RELAY_TOG:
        g_opt.mouseRelay = g_opt.mouseRelay ? 0 : 1;
        Log("relay: 中继开关 -> %s（仍是按需接管：只有证明客户端在吞事件时才动手）",
            g_opt.mouseRelay ? "开" : "关");
        break;
    case IDM_RELAY_ACCEL:
        RelaySetAccel(!RelayGetAccel());
        RelaySaveSettings();      // 记住选择（HKCU\Software\OeWinFix2\MouseAccel）
        break;
    case IDM_BCAST_FIX:
        g_opt.bcastFix = g_opt.bcastFix ? 0 : 1;
        if (g_opt.bcastFix) BcastStart(); else BcastStop();
        Log("bcast: 改写功能已%s", g_opt.bcastFix ? "打开" : "关闭");
        break;
    case IDM_OPEN_LOG: {
        wchar_t dir[MAX_PATH]; wcsncpy_s(dir, MAX_PATH, LogFilePath(), _TRUNCATE);
        wchar_t* s = wcsrchr(dir, L'\\'); if (s) *s = 0;
        ShellExecuteW(NULL, L"open", dir, NULL, NULL, SW_SHOWNORMAL);
        break;
    }
    case IDM_EXIT: g_realExit = TRUE; PostMessageW(h, WM_CLOSE, 0, 0); break;
    }
}

// ---------------------------------------------------------------- 隐藏窗口（收托盘消息 + 热键）
static LRESULT CALLBACK HiddenProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    // 资源管理器重启会重建通知区 -> 图标得自己补回去（否则用户以为程序没了）
    if (g_wmTaskbarCreated && m == g_wmTaskbarCreated) {
        Log("tray: 收到 TaskbarCreated（资源管理器重启）-> 重新添加托盘图标");
        g_trayAdded = FALSE;
        TrayAdd(h);
        return 0;
    }
    // 另一个实例请求我们把面板弹出来
    if (m == OWF2_MSG_SHOWUI) {
        Log("ipc: 收到已有实例的“弹面板”请求");
        StatusUiShow();
        return 0;
    }
    // 另一个实例请我们退出（--quit）
    if (m == OWF2_MSG_QUIT) {
        Log("ipc: 收到“退出”请求 -> 正常退出");
        AppRequestExit();
        return 0;
    }
    switch (m) {
    case WM_TRAY:
        if (LOWORD(l) == WM_RBUTTONUP) TrayMenu(h);
        else if (LOWORD(l) == WM_LBUTTONDBLCLK) StatusUiShow();
        return 0;
    case WM_HOTKEY:
        if (w == HOTKEY_FIX) {
            int n = WindowizeNowManual();
            Log("hotkey Ctrl+Alt+F9 -> 窗口化 %d", n);
        } else if (w == HOTKEY_PANIC) {
            Log("hotkey Ctrl+Alt+F10 -> 紧急解封（窗口化 + 键盘释放 + 夺回桌面 + 中继）");
            WindowizeNowManual();
            KbdReleaseOnce();
            if (!DesktopIsOurs()) ReclaimOwnDesktop();
            if (g_opt.mouseRelay && !RelayIsOn()) { g_opt.relayForce = 1; }
        }
        return 0;
    case WM_CLOSE:
        // 广播期间客户端会给不相关窗口群发 WM_CLOSE（实测），绝不能让它把守护线程带走：
        // 只有托盘菜单点了"退出程序"才真退出。
        if (!g_realExit) {
            static DWORD lastLog = 0;
            DWORD now = GetTickCount();
            if (now - lastLog > 10000) {
                Log("收到 WM_CLOSE（多半是客户端在广播期间扫窗口）-> 忽略，守护继续；退出请用托盘菜单");
                lastLog = now;
            }
            return 0;
        }
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ---------------------------------------------------------------- CLI（单表解析）
typedef struct { const wchar_t* name; int* target; int value; } FLAG;
typedef struct { const wchar_t* name; int* target; } NUMOPT;

static void Usage(void)
{
    printf(
"OeWinFix2 — 教学系统学生端：① 全屏广播窗口化  ② 键盘鼠标恢复正常（+ 镜像窗口）\n"
"\n"
"用法: OeWinFix2.exe [选项]        （双击 = 托盘守护；本程序需要管理员权限）\n"
"\n"
"功能开关（默认全开）:\n"
"  --no-windowize           不做窗口化\n"
"  --no-desktop             不夺回自己的输入桌面\n"
"  --no-mirror              不开镜像窗口\n"
"  --relay                  打开鼠标中继（默认**就开着**：探测驱动 —— 只有证明客户端在吞事件\n"
"                           时才接管，平时零钩子零注入。接管方式是直驱光标，不经过钩子链）\n"
"  --no-relay               关掉中继（只用 bcast 改写 + 键盘 + 桌面，鼠标交给系统）\n"
"  --accel on|off           鼠标加速曲线：off = 线性 1:1（默认，灵敏度低但可预测）；\n"
"                           on = 复刻系统\"提高指针精确度\"（快速甩动放大 2~4 倍）。会存 HKCU。\n"
"  --sens <倍数>            鼠标灵敏度倍数 0.2~4.0（默认 1.0 = 设备计数与像素 1:1）。会存 HKCU。\n"
"  --bcast-fix / --no-bcast-fix  开/关 全屏广播改写(默认开: #fullscreen#:1 -> 0)\n"
"  --no-kbd                 不做键盘释放\n"
"  --no-tray                不加托盘图标（配合 --once 或脚本用）\n"
"\n"
"周期/几何:\n"
"  --interval <ms>          窗口化纠正周期(默认 200)\n"
"  --desktop-interval <ms>  桌面检测周期(默认 250)\n"
"  --kbd-interval <ms>      键盘释放周期(默认 400)\n"
"  --mirror-fps <n>         镜像帧率(默认 10)\n"
"  --w <px> --h <px>        窗口化后的尺寸(默认工作区 80%%)\n"
"  --x <px> --y <px>        窗口化后的位置(默认居中)\n"
"  --resize                 允许守护自动改尺寸位置（默认不允许；手动窗口化不受影响）\n"
"\n"
"诊断:\n"
"  --status                 打印状态后退出\n"
"  --windowize-now          立刻窗口化一次后退出\n"
"  --quit                   请已在运行的实例正常退出（不用去找托盘图标）\n"
"  --panel / --no-panel     启动时是否弹状态面板（默认弹）\n"
"  --relay-force            不等检测到广播，直接接管鼠标中继(测试用)\n"
"  --once                   各线程只跑一轮(自检)\n"
"  --verbose                详细日志\n"
"  --log <路径>             指定日志文件\n"
"  --help                   这个帮助\n");
}

static BOOL ParseArgs(int argc, wchar_t** argv)
{
    FLAG flags[] = {
        { L"--no-windowize",  &g_opt.windowize,       0 },
        { L"--no-desktop",    &g_opt.desktopReclaim,  0 },
        { L"--no-mirror",     &g_opt.mirror,          0 },
        { L"--no-relay",      &g_opt.mouseRelay,      0 },
        { L"--relay",         &g_opt.mouseRelay,      1 },
        { L"--no-accel",      &g_opt.mouseAccel,      0 },
        { L"--accel-on",      &g_opt.mouseAccel,      1 },
        { L"--no-bcast-fix",  &g_opt.bcastFix,        0 },
        { L"--bcast-fix",     &g_opt.bcastFix,        1 },
        { L"--no-kbd",        &g_opt.keyboard,        0 },
        { L"--no-tray",       &g_opt.tray,            0 },
        { L"--no-resize",     &g_opt.noResize,        1 },
        { L"--resize",        &g_opt.noResize,        0 },
        { L"--verbose",       &g_opt.verbose,         1 },
        { L"--once",          &g_opt.once,            1 },
        { L"--relay-force",   &g_opt.relayForce,      1 },
        { L"--status",        &g_opt.showStatus,      1 },
        { L"--windowize-now", &g_opt.windowizeNow,    1 },
        { L"--quit",          &g_opt.quit,            1 },
        { L"--panel",         &g_opt.showPanel,       1 },
        { L"--no-panel",      &g_opt.showPanel,       0 },
    };
    NUMOPT nums[] = {
        { L"--interval",         &g_opt.windowIntervalMs  },
        { L"--desktop-interval", &g_opt.desktopIntervalMs },
        { L"--kbd-interval",     &g_opt.kbdIntervalMs     },
        { L"--mirror-fps",       &g_opt.mirrorFps         },
        { L"--w",                &g_opt.winW              },
        { L"--h",                &g_opt.winH              },
        { L"--x",                &g_opt.winX              },
        { L"--y",                &g_opt.winY              },
    };
    for (int i = 1; i < argc; i++) {
        if (_wcsicmp(argv[i], L"--help") == 0) { Usage(); return FALSE; }
        if (_wcsicmp(argv[i], L"--log") == 0 && i + 1 < argc) {
            wcsncpy_s(g_logPath, MAX_PATH, argv[++i], _TRUNCATE);
            continue;
        }
        if (_wcsicmp(argv[i], L"--accel") == 0 && i + 1 < argc) {
            g_opt.mouseAccel = (_wcsicmp(argv[++i], L"off") == 0) ? 0 : 1;
            continue;
        }
        if (_wcsicmp(argv[i], L"--sens") == 0 && i + 1 < argc) {
            g_sensOpt = _wtof(argv[++i]);
            continue;
        }
        BOOL hit = FALSE;
        for (int k = 0; k < (int)(sizeof(flags) / sizeof(flags[0])); k++) {
            if (_wcsicmp(argv[i], flags[k].name) == 0) { *flags[k].target = flags[k].value; hit = TRUE; break; }
        }
        if (hit) continue;
        for (int k = 0; k < (int)(sizeof(nums) / sizeof(nums[0])); k++) {
            if (_wcsicmp(argv[i], nums[k].name) == 0 && i + 1 < argc) {
                *nums[k].target = _wtoi(argv[++i]);
                hit = TRUE; break;
            }
        }
        if (!hit) Log("警告: 未知参数 %ls（--help 看全部）", argv[i]);
    }
    return TRUE;
}

// ---------------------------------------------------------------- main
// WINDOWS 子系统 exe：入口是 wWinMain（wmain 会找不到 WinMain 而链接失败）
static int RunApp(int argc, wchar_t** argv)
{
    BOOL wantHelp = FALSE;
    for (int i = 1; i < argc; i++) if (_wcsicmp(argv[i], L"--help") == 0) wantHelp = TRUE;

    // 需要控制台时才申请（GUI 子系统 exe）
    if (wantHelp || argc > 1) {
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
        FILE* f = NULL;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }
    if (wantHelp) { Usage(); return 0; }
    if (!ParseArgs(argc, argv)) return 0;

    LogInit(g_logPath[0] ? g_logPath : NULL);

    // 鼠标加速曲线：默认沿用上次的选择（HKCU\Software\OeWinFix2\MouseAccel），命令行 --accel 可覆盖
    RelayLoadSettings();
    if (g_opt.mouseAccel >= 0) { RelaySetAccel(g_opt.mouseAccel); RelaySaveSettings(); }
    if (g_sensOpt > 0.0)       { RelaySetSens(g_sensOpt);         RelaySaveSettings(); }

    DWORD sess = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &sess);
    Log("OeWinFix2 start (pid=%u, elevated=%d, session=%u)",
        (unsigned)GetCurrentProcessId(), IsElevated(), (unsigned)sess);
    if (!IsElevated())
        Log("警告: 当前不是管理员权限 —— 键盘 IOCTL 会失败，且可能无法向高完整性窗口注入输入（请以管理员运行）");
    EnablePrivilege(L"SeDebugPrivilege");
    EnablePrivilege(L"SeLoadDriverPrivilege");

    Log("自检: 自己桌面='%ls' 输入桌面='%ls' KbdFilter=%ls 广播窗口=%ls",
        OwnDesktopName(), InputDesktopName(),
        KbdAvailable() ? L"可打开" : L"不可用",
        HasBroadcastWindow() ? L"有" : L"无");

    // 一次性命令
    if (g_opt.windowizeNow) {
        int n = WindowizeNowManual();
        Log("windowize-now: 处理 %d 个窗口", n);
        if (IsInteractiveSession() && argc > 1) { printf("处理了 %d 个窗口\n", n); Sleep(500); }
        LogClose(); return 0;
    }
    if (g_opt.showStatus) {
        wchar_t s[2048];
        StatusUiBuildText(s, 2048);
        LogW(s);
        wprintf(L"%ls\n", s);
        LogClose(); return 0;
    }
    if (g_opt.quit) {
        // 不用找托盘图标：让已有实例自己走正常退出流程
        BOOL ok = AskRunningQuit(4000);
        Log("quit: %s", ok ? "已有实例已退出" : "没有在运行的实例（或对端没响应）");
        LogClose(); return ok ? 0 : 1;
    }

    if (!AcquireSingleInstance()) {
        // 已有实例在跑：请它把自己的状态面板弹出来。
        // 以前这里只弹一个 MessageBox —— 用户看到"有弹窗但没有托盘程序"，还找不到怎么退出。
        BOOL handed = AskRunningShowPanel(1200);        Log("另一个 OeWinFix2 已在运行 -> %s", handed ? "已请它弹出状态面板" : "弹状态框后退出");
        if (!handed) ShowStatusBox();   // 旧版本/对端不响应时退回弹框
        LogClose(); return 0;
    }

    g_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    // 启动各守护线程
    if (g_opt.windowize)   StartWorker(WindowizeProc, NULL, "windowize");
    if (g_opt.desktopReclaim) StartWorker(DesktopProc, NULL, "desktop");
    if (g_opt.keyboard)    StartWorker(KeyboardProc, NULL, "keyboard");
    if (g_opt.mirror)      MirrorStart();
    RelayStart();   // 始终启动: mouseRelay=0 时只做只读监视(报告客户端是否在吞事件)
    if (g_opt.bcastFix)    BcastStart();

    if (g_opt.once) {
        Sleep(2000);
        StopAllWorkers(3000);
        RelayStop(); BcastStop(); MirrorStop();
        Log("--once 自检结束");
        LogClose(); return 0;
    }

    // 托盘 + 热键（需要一个消息循环）
    // 注意: 托盘宿主必须是**真实窗口**，不能用 HWND_MESSAGE 消息专用窗口 ——
    // 用它挂 Shell_NotifyIcon 会让通知区（含"隐藏图标"溢出面板）失灵（我第一版就踩了）。
    WNDCLASSEXW wc; memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HiddenProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"OeWinFix2Status";
    RegisterClassExW(&wc);
    g_hidden = CreateWindowExW(WS_EX_TOOLWINDOW, L"OeWinFix2Status", L"OeWinFix2", WS_POPUP,
                               0, 0, 0, 0, NULL, NULL, wc.hInstance, NULL);
    // 故意不 ShowWindow：它只是个消息宿主
    if (g_opt.tray && g_hidden) TrayAdd(g_hidden);
    BOOL hk1 = RegisterHotKey(g_hidden, HOTKEY_FIX,   MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F9);
    BOOL hk2 = RegisterHotKey(g_hidden, HOTKEY_PANIC, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F10);
    BOOL hk8 = RegisterHotKey(g_hidden, 0xB008, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F8);
    Log("hotkeys: F9(立刻窗口化)=%d F10(紧急解封)=%d F8(看大画面:铺满/还原)=%d", hk1, hk2, hk8);
    Log("已进入守护状态（双击托盘图标看状态面板；面板里或右键菜单可退出）");

    // 默认把面板摆出来：只靠托盘图标的话，图标一进"隐藏的图标"就等于没有 UI
    if (g_opt.showPanel) StatusUiShow();

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_HOTKEY && msg.wParam == 0xB008) {
            // F8：B 方案生效 = "看大画面"（广播窗口铺满/还原）；没生效 = 课堂桌面来回切
            if (HasBroadcastWindow() && !BroadcastWindowOnOwnDesktop()) {
                if (DesktopIsOurs()) { if (SwitchToClassDesktop()) DesktopHoldReclaim(60000); }
                else ReclaimOwnDesktop();
            } else if (WindowizeToggleMaximize() == 0) {
                Log("hotkey F8: 当前没有广播窗口可放大");
            }
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    Log("exit: 停止所有线程");
    StatusUiClose();
    RelayStop();
    BcastStop();
    MirrorStop();
    StopAllWorkers(3000);
    TrayRemove();
    Log("OeWinFix2 exit");
    LogClose();
    return 0;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int rc = RunApp(argc, argv ? argv : NULL);
    if (argv) LocalFree(argv);
    return rc;
}
