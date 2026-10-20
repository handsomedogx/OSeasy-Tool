// ============================================================================
//  relay.cpp — 鼠标中继（直驱光标）
//
//  要解决的问题（实测，见 src/facts.h）：
//    客户端广播期间装 WH_MOUSE_LL，只放行 dwExtraInfo == 0x12452 的事件，其余 return 1 吞掉。
//    窗口化（bcast 模块）只解决"输入桌面归谁"，没有让客户端卸掉这个钩子 —— 所以鼠标仍然是死的。
//
//  做法（全程只在本进程内，不碰客户端内存/驱动/文件）：
//    1) 自己的 WH_MOUSE_LL：把非 0x12452 的原件抢先丢掉；
//    2) 隐藏窗口收 Raw Input（不受钩子影响）：取**设备原始增量**；
//    3) 用 SetCursorPos 直接设绝对坐标 —— 不经过钩子链、不排队、不过系统加速曲线。
//
//  为什么用 SetCursorPos 而不是 SendInput 重放（2026-10-20 实测，重要）：
//    * SendInput 补出来的事件必须**穿过客户端的低级钩子**才生效，钩子链是串行的；
//      鼠标 125Hz+ 时我方发送速度超过链条消化速度 -> 事件在输入队列里积压，
//      总位移没错但被匀速排空，观感是"实际移了一大段、指针在后面慢慢追上来"。
//    * SetCursorPos 不进钩子链、不排队，一次调用立即到位：实测延迟消失、完全跟手。
//      （同一段操作：旧法 5 秒往链上塞 975 个事件，直驱只有 4 个 —— 全是别人发的。）
//
//  加速曲线（可开关，默认关）：
//    系统的"提高指针精确度"会按速度放大位移（阈值 6/10 -> x2/x4）。直驱绕过了它，
//    所以这里自己复刻：把移动量按 dt 归一成"每 10ms 的计数"当速度，再比阈值。
//    归一很关键 —— 不除以 dt 的话慢速也会被判成高速（实测 x4 命中 264/277，指针会飞）。
//    默认关 = 线性、低灵敏度（用户偏好）；开关状态存 HKCU\Software\OeWinFix2\MouseAccel。
//
//  安全阀（任一触发都不会让鼠标变死）：
//    * 只有"raw 通路就绪"且"正在接管"才丢原件，缺一个就原样放行；
//    * 输入桌面不是自己（广播抢走了桌面）-> 立刻停止接管（这时注入会打到别的桌面上）；
//    * 连续设置光标失败 30 次 -> 立刻停止接管；
//    * 广播结束（没有广播窗口）一段时间后自动停止接管并卸钩子，回到零痕迹状态。
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#include "relay.h"
#include "config.h"
#include "facts.h"
#include "log.h"
#include "util.h"
#include "windowize.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <wchar.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "kernel32.lib")

#define SETTINGS_KEY      L"Software\\OeWinFix2"

// ---------------------------------------------------------------- 状态
static volatile LONG g_on = 0;         // 正在接管
static volatile LONG g_ready = 0;      // raw sink 就绪
static volatile LONG g_absolute = 0;   // 设备报绝对坐标（远程/虚拟鼠标）-> 不接管
static volatile LONG g_swallow = 0;    // 探测结论：客户端在吞事件
static volatile LONG g_fail = 0;       // 连续设置光标失败
static volatile LONG g_stop = 0;
static volatile LONG g_rawPkts = 0, g_pktsIgnored = 0, g_relayed = 0, g_buttons = 0, g_hookSeen = 0;
static volatile LONG g_clamped = 0, g_gain2 = 0, g_gain4 = 0;

// 直驱光标
static double g_vx = 0, g_vy = 0;
static volatile LONG g_vvalid = 0;
static volatile LONG g_lastMoveTick = 0;

// 加速曲线
static volatile LONG g_accel = 0;      // 0 = 线性（默认，用户偏好）；1 = 复刻系统加速
static volatile LONG g_sens100 = 100;  // 灵敏度倍数 x100（100 = 1:1）
static int    g_t1 = 6, g_t2 = 10;
static double g_vel = 0;
static LARGE_INTEGER g_qpf, g_lastQpc;

// 线程/句柄
static HANDLE g_sinkThread = NULL, g_hookThread = NULL, g_monThread = NULL;
static HWND   g_sinkWnd = NULL;
static HHOOK  g_hook = NULL;
static DWORD  g_sinkTid = 0, g_hookTid = 0;
static HANDLE g_hookInstalled = NULL;
static BOOL   g_started = FALSE;

// 按键
static LONG  g_downX = -1, g_downY = -1;
static DWORD g_downTick = 0;

// ---------------------------------------------------------------- 设置读写
static int RegReadInt(const wchar_t* name, int def)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, SETTINGS_KEY, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return def;
    wchar_t buf[64] = L"";
    DWORD cb = (DWORD)sizeof(buf) - sizeof(wchar_t), type = 0;
    LONG r = RegQueryValueExW(k, name, NULL, &type, (LPBYTE)buf, &cb);
    RegCloseKey(k);
    if (r != ERROR_SUCCESS) return def;
    if (type == REG_SZ) return _wtoi(buf);
    if (type == REG_DWORD) return (int)*(DWORD*)buf;
    return def;
}

static void RegWriteInt(const wchar_t* name, int v)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, SETTINGS_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS)
        return;
    DWORD d = (DWORD)v;
    RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&d, sizeof(d));
    RegCloseKey(k);
}

void RelayLoadSettings(void)
{
    InterlockedExchange(&g_accel, RegReadInt(L"MouseAccel", 0) ? 1 : 0);   // 默认关：线性、低灵敏度
    int s = RegReadInt(L"MouseSens100", 100);
    if (s < 20) s = 20;
    if (s > 400) s = 400;
    InterlockedExchange(&g_sens100, s);
}

void RelaySaveSettings(void)
{
    RegWriteInt(L"MouseAccel", g_accel != 0);
    RegWriteInt(L"MouseSens100", g_sens100);
}

void RelaySetSens(double factor)
{
    if (factor < 0.2) factor = 0.2;
    if (factor > 4.0) factor = 4.0;
    InterlockedExchange(&g_sens100, (LONG)(factor * 100.0 + 0.5));
    Log("relay: 灵敏度 = x%.2f", g_sens100 / 100.0);
}

double RelayGetSens(void) { return g_sens100 / 100.0; }

void RelaySetAccel(int on)
{
    InterlockedExchange(&g_accel, on ? 1 : 0);
    g_vel = 0;
    Log("relay: 加速曲线 %s（%s）", g_accel ? "开" : "关",
        g_accel ? "复刻系统'提高指针精确度'：快速甩动放大 2~4 倍"
                : "线性 1:1，灵敏度低但完全可预测");
}

int RelayGetAccel(void) { return g_accel != 0; }

// ---------------------------------------------------------------- 直驱光标
static void DriveCursorAbs(LONG dx, LONG dy)
{
    int sx = GetSystemMetrics(SM_XVIRTUALSCREEN), sy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int sw = GetSystemMetrics(SM_CXVIRTUALSCREEN), sh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (sw <= 1) { sw = GetSystemMetrics(SM_CXSCREEN); sx = 0; }
    if (sh <= 1) { sh = GetSystemMetrics(SM_CYSCREEN); sy = 0; }

    if (!g_vvalid) {                      // 首次/空闲后重新同步真实光标，避免越走越偏
        POINT p;
        if (GetCursorPos(&p)) { g_vx = p.x; g_vy = p.y; }
        InterlockedExchange(&g_vvalid, 1);
        g_vel = 0;
    }

    double gain = g_sens100 / 100.0;    // 灵敏度倍数（线性模式下就是整体快慢）
    if (g_accel) {
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        double dt_ms = 8.0;
        if (g_lastQpc.QuadPart && g_qpf.QuadPart)
            dt_ms = (double)(now.QuadPart - g_lastQpc.QuadPart) * 1000.0 / (double)g_qpf.QuadPart;
        if (dt_ms <= 0.0) dt_ms = 0.5;
        if (dt_ms > 30.0) { g_vel *= 0.5; dt_ms = 30.0; }
        g_lastQpc = now;

        double mag  = fabs((double)dx) + fabs((double)dy);
        double inst = mag * 10.0 / dt_ms;                  // 归一到"每 10ms 的计数"
        g_vel = g_vel * 0.5 + inst * 0.5;

        if (g_vel > (double)g_t2)      { gain *= 4.0; InterlockedIncrement(&g_gain4); }
        else if (g_vel > (double)g_t1) { gain *= 2.0; InterlockedIncrement(&g_gain2); }
    }

    g_vx += dx * gain;
    g_vy += dy * gain;

    // 撞边要把虚拟位置一起夹住，否则回头时要先"还债"，表现为反向不动一下
    double lox = sx, hix = sx + sw - 1, loy = sy, hiy = sy + sh - 1;
    if (g_vx < lox) { g_vx = lox; InterlockedIncrement(&g_clamped); }
    if (g_vx > hix) { g_vx = hix; InterlockedIncrement(&g_clamped); }
    if (g_vy < loy) { g_vy = loy; InterlockedIncrement(&g_clamped); }
    if (g_vy > hiy) { g_vy = hiy; InterlockedIncrement(&g_clamped); }

    int nx = (int)(g_vx + 0.5), ny = (int)(g_vy + 0.5);
    static int lastx = -99999, lasty = -99999;   // 像素没变就不调，省调用
    if (nx != lastx || ny != lasty) {
        if (SetCursorPos(nx, ny)) {
            lastx = nx; lasty = ny;
            InterlockedExchange(&g_fail, 0);
            InterlockedIncrement(&g_relayed);
        } else {
            InterlockedIncrement(&g_fail);
        }
    }
    InterlockedExchange(&g_lastMoveTick, (LONG)GetTickCount());
}

// ---------------------------------------------------------------- 按键（仍要走真实输入）
// 按键没法用 SetCursorPos 代替：must 作为输入事件送达窗口。带 0x12452 标记，客户端白名单放行。
static void InjectButtonAt(DWORD flag, POINT pt, DWORD data)
{
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 1) { vw = GetSystemMetrics(SM_CXSCREEN); vx = 0; }
    if (vh <= 1) { vh = GetSystemMetrics(SM_CYSCREEN); vy = 0; }

    INPUT in; memset(&in, 0, sizeof(in));
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flag | MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    in.mi.dx = (LONG)((double)(pt.x - vx) * 65535.0 / (vw - 1));
    in.mi.dy = (LONG)((double)(pt.y - vy) * 65535.0 / (vh - 1));
    in.mi.mouseData = data;
    in.mi.dwExtraInfo = MOUSE_MAGIC;
    if (SendInput(1, &in, sizeof(in)) == 0) InterlockedIncrement(&g_fail);
    else InterlockedExchange(&g_fail, 0);
    InterlockedIncrement(&g_buttons);
}

static void InjectButtonFlag(DWORD flag, DWORD data)
{
    BOOL isDown = (flag == MOUSEEVENTF_LEFTDOWN || flag == MOUSEEVENTF_RIGHTDOWN ||
                   flag == MOUSEEVENTF_MIDDLEDOWN || flag == MOUSEEVENTF_XDOWN);
    BOOL isBtn  = isDown || flag == MOUSEEVENTF_LEFTUP || flag == MOUSEEVENTF_RIGHTUP ||
                  flag == MOUSEEVENTF_MIDDLEUP || flag == MOUSEEVENTF_XUP;

    POINT pt; GetCursorPos(&pt);
    if (!isBtn) { InjectButtonAt(flag, pt, data); return; }   // 滚轮：原地滚

    if (isDown) {
        DWORD now = GetTickCount();
        // 双击锁定：时限内且位置很近 -> 用上次按下的坐标，保证两次点在同一像素
        if (g_downX >= 0 && (now - g_downTick) <= GetDoubleClickTime() &&
            labs(pt.x - g_downX) <= 6 && labs(pt.y - g_downY) <= 6) {
            pt.x = g_downX; pt.y = g_downY;
        } else {
            g_downX = pt.x; g_downY = pt.y;
        }
        g_downTick = now;
    } else if (g_downX >= 0) {
        if (labs(pt.x - g_downX) <= 6 && labs(pt.y - g_downY) <= 6) { pt.x = g_downX; pt.y = g_downY; }
    }
    InjectButtonAt(flag, pt, data);

    // 按键把光标钉在这个点上了 -> 虚拟位置跟着对齐，避免下次移动跳一下
    g_vx = pt.x; g_vy = pt.y;
    InterlockedExchange(&g_vvalid, 1);
}

static void InjectRawButtons(USHORT flags, USHORT data)
{
    static const struct { USHORT bit; DWORD flag; DWORD md; } map[] = {
        { RI_MOUSE_LEFT_BUTTON_DOWN,   MOUSEEVENTF_LEFTDOWN,   0 },
        { RI_MOUSE_LEFT_BUTTON_UP,     MOUSEEVENTF_LEFTUP,     0 },
        { RI_MOUSE_RIGHT_BUTTON_DOWN,  MOUSEEVENTF_RIGHTDOWN,  0 },
        { RI_MOUSE_RIGHT_BUTTON_UP,    MOUSEEVENTF_RIGHTUP,    0 },
        { RI_MOUSE_MIDDLE_BUTTON_DOWN, MOUSEEVENTF_MIDDLEDOWN, 0 },
        { RI_MOUSE_MIDDLE_BUTTON_UP,   MOUSEEVENTF_MIDDLEUP,   0 },
        { RI_MOUSE_BUTTON_4_DOWN,      MOUSEEVENTF_XDOWN,      XBUTTON1 },
        { RI_MOUSE_BUTTON_4_UP,        MOUSEEVENTF_XUP,        XBUTTON1 },
        { RI_MOUSE_BUTTON_5_DOWN,      MOUSEEVENTF_XDOWN,      XBUTTON2 },
        { RI_MOUSE_BUTTON_5_UP,        MOUSEEVENTF_XUP,        XBUTTON2 },
    };
    for (int i = 0; i < (int)(sizeof(map) / sizeof(map[0])); i++)
        if (flags & map[i].bit) InjectButtonFlag(map[i].flag, map[i].md);
    if (flags & RI_MOUSE_WHEEL)  InjectButtonFlag(MOUSEEVENTF_WHEEL,  (DWORD)(SHORT)data);
    if (flags & RI_MOUSE_HWHEEL) InjectButtonFlag(MOUSEEVENTF_HWHEEL, (DWORD)(SHORT)data);
}

// ---------------------------------------------------------------- 钩子线程（纯泵，绝不阻塞）
static LRESULT CALLBACK HookProc(int nCode, WPARAM w, LPARAM l)
{
    if (nCode == HC_ACTION) {
        InterlockedIncrement(&g_hookSeen);
        // 丢原件必须同时满足：正在接管 + raw 通路活着。缺一个就原样放行（否则就是死鼠标）。
        if (g_on && g_ready) {
            const MSLLHOOKSTRUCT* ms = (const MSLLHOOKSTRUCT*)l;
            if (ms->dwExtraInfo != MOUSE_MAGIC) return 1;
        }
    }
    return CallNextHookEx(g_hook, nCode, w, l);
}

static DWORD WINAPI HookThreadProc(LPVOID)
{
    g_hookTid = GetCurrentThreadId();
    g_hook = SetWindowsHookExW(WH_MOUSE_LL, HookProc, NULL, 0);
    InterlockedExchange(&g_hookSeen, 0);
    SetEvent(g_hookInstalled);
    if (!g_hook) { Log("relay: SetWindowsHookEx(WH_MOUSE_LL) 失败 err=%lu", GetLastError()); return 0; }

    // 本线程**只能**是消息循环：低级钩子的回调是系统在这期间投递的。
    // 任何 Sleep / 写盘 / 探测都不能出现在这里 —— 一阻塞，全系统鼠标跟着停。
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

static BOOL EnsureHook(BOOL want)
{
    if (want && !g_hookThread) {
        ResetEvent(g_hookInstalled);
        g_hookThread = CreateThread(NULL, 0, HookThreadProc, NULL, 0, NULL);
        WaitForSingleObject(g_hookInstalled, 2000);
        if (!g_hook) {
            if (g_hookThread) { WaitForSingleObject(g_hookThread, 1000); CloseHandle(g_hookThread); g_hookThread = NULL; }
            return FALSE;
        }
        Log("relay: WH_MOUSE_LL 已装（钩子线程只做丢弃判断，不睡不写盘）");
        return TRUE;
    }
    if (!want && g_hookThread) {
        if (g_hook) { UnhookWindowsHookEx(g_hook); g_hook = NULL; }
        if (g_hookTid) PostThreadMessageW(g_hookTid, WM_QUIT, 0, 0);
        WaitForSingleObject(g_hookThread, 2000);
        CloseHandle(g_hookThread);
        g_hookThread = NULL;
        Log("relay: 不再需要接管 -> 卸掉 WH_MOUSE_LL（输入不再经过我们，零影响）");
        return FALSE;
    }
    return g_hook != NULL;
}

// ---------------------------------------------------------------- raw 收包（注入在这里）
static LRESULT CALLBACK SinkProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_INPUT) {
        UINT size = 0;
        GetRawInputData((HRAWINPUT)l, RID_INPUT, NULL, &size, sizeof(RAWINPUTHEADER));
        if (size && size <= 512) {
            BYTE buf[512];
            UINT got = GetRawInputData((HRAWINPUT)l, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER));
            if (got == size) {
                RAWINPUT* ri = (RAWINPUT*)buf;
                if (ri->header.dwType == RIM_TYPEMOUSE) {
                    const RAWMOUSE* rm = &ri->data.mouse;
                    InterlockedIncrement(&g_rawPkts);
                    if (ri->header.hDevice == NULL) {          // 注入输入（含别人的）：忽略，防反馈环
                        InterlockedIncrement(&g_pktsIgnored);
                        return 0;
                    }
                    if (rm->usFlags & MOUSE_MOVE_ABSOLUTE) {
                        InterlockedExchange(&g_absolute, 1);   // 远程/虚拟鼠标：中继无意义
                    } else if (g_on && (rm->lLastX || rm->lLastY)) {
                        DriveCursorAbs(rm->lLastX, rm->lLastY);
                    }
                    if (rm->usButtonFlags && g_on) InjectRawButtons(rm->usButtonFlags, rm->usButtonData);
                }
            }
        }
        return 0;
    }
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

static DWORD WINAPI SinkThreadProc(LPVOID)
{
    g_sinkTid = GetCurrentThreadId();
    WNDCLASSEXW wc; memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = SinkProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"OeWinFix2RawSink";
    RegisterClassExW(&wc);
    g_sinkWnd = CreateWindowExW(0, L"OeWinFix2RawSink", L"", WS_POPUP, 0, 0, 0, 0, NULL, NULL, wc.hInstance, NULL);
    if (!g_sinkWnd) { Log("relay: raw sink 窗口创建失败 err=%lu", GetLastError()); return 0; }

    RAWINPUTDEVICE rid;
    rid.usUsagePage = 0x01; rid.usUsage = 0x02;
    rid.dwFlags = RIDEV_INPUTSINK; rid.hwndTarget = g_sinkWnd;
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        Log("relay: RegisterRawInputDevices 失败 err=%lu -> 保持透明（不会吞任何输入）", GetLastError());
        DestroyWindow(g_sinkWnd); g_sinkWnd = NULL; return 0;
    }
    InterlockedExchange(&g_ready, 1);
    Log("relay: raw sink ready（隐藏窗口 + RIDEV_INPUTSINK）");

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    InterlockedExchange(&g_ready, 0);
    if (g_sinkWnd) { DestroyWindow(g_sinkWnd); g_sinkWnd = NULL; }
    return 0;
}

// ---------------------------------------------------------------- 差分探测（只在本线程跑，本线程无钩子）
static BOOL InjectAndWait(DWORD extra, POINT* before)
{
    INPUT in; memset(&in, 0, sizeof(in));
    in.type = INPUT_MOUSE;
    in.mi.dx = 3; in.mi.dy = 0;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    in.mi.dwExtraInfo = (ULONG_PTR)extra;
    if (SendInput(1, &in, sizeof(in)) == 0) return FALSE;
    for (int i = 0; i < 120; i++) {           // 最多 120ms
        Sleep(1);
        POINT c; GetCursorPos(&c);
        if (c.x != before->x || c.y != before->y) return TRUE;
    }
    return FALSE;
}

// 普通注入动不了、0x12452 能动能 -> 客户端确实在吞事件（差分，避免误报）
static BOOL ProbeSwallowing(void)
{
    POINT a, b;
    GetCursorPos(&a);
    Sleep(30);
    GetCursorPos(&b);
    if (a.x != b.x || a.y != b.y) return (g_swallow != 0);   // 光标在动 -> 维持上次结论

    POINT p0; GetCursorPos(&p0);
    BOOL plainMoved = InjectAndWait(0, &p0);
    SetCursorPos(p0.x, p0.y);                                // 还原（3px，不可见）

    Sleep(20);
    POINT p1; GetCursorPos(&p1);
    BOOL magicMoved = InjectAndWait(MOUSE_MAGIC, &p1);
    SetCursorPos(p1.x, p1.y);

    if (!plainMoved && !magicMoved) return (g_swallow != 0); // 判断不了，维持上次结论
    return (!plainMoved && magicMoved) ? TRUE : FALSE;
}

// ---------------------------------------------------------------- 决策线程
static void Arm(int why)
{
    if (g_on) return;
    if (!g_ready)     { Log("relay: 暂不接管（raw 通路未就绪，宁可不动也不让鼠标变死）"); return; }
    if (g_absolute)   { Log("relay: 暂不接管（设备报绝对坐标，远程/虚拟鼠标，中继无意义）"); return; }
    InterlockedExchange(&g_on, 1);
    InterlockedExchange(&g_vvalid, 0);
    g_downX = g_downY = -1;
    Log("relay: 状态 -> 接管中（%s）", why == 1 ? "探测确认客户端在吞事件；直驱光标，不经过钩子链"
                                                : "手动开启；直驱光标，不经过钩子链");
}

static void Disarm(const char* why)
{
    if (!g_on) return;
    InterlockedExchange(&g_on, 0);
    Log("relay: 状态 -> 透明（%s）", why);
}

static void LogStats(const char* why)
{
    Log("relay[%s]: raw=%ld(忽略注入包 %ld) 位移=%ld 按键=%ld 钩子可见=%ld 夹边=%ld 加速=%s(x2=%ld,x4=%ld) 灵敏度=x%.2f 状态=%s",
        why, (long)g_rawPkts, (long)g_pktsIgnored, (long)g_relayed, (long)g_buttons, (long)g_hookSeen,
        (long)g_clamped, g_accel ? "开" : "关", (long)g_gain2, (long)g_gain4, g_sens100 / 100.0,
        g_on ? "接管中" : (g_absolute ? "透明(绝对坐标设备)" : (g_swallow ? "透明(待命)" : "透明(客户端没吞事件)")));
}

static DWORD WINAPI MonThreadProc(LPVOID)
{
    QueryPerformanceFrequency(&g_qpf);
    Log("relay: monitor on（探测驱动；加速曲线=%s%s）", g_accel ? "开" : "关",
        g_opt.relayForce ? "；--relay-force 强制接管" : "");

    int ticks = 0, noBcast = 0;
    DWORD lastProbe = 0;

    while (!g_stop) {
        DWORD now = GetTickCount();

        // 0) 手动关掉中继 -> 立刻收手
        if (!g_opt.mouseRelay && g_on) Disarm("中继已手动关闭");

        // 1) 钩子只在"需要接管"或"已接收管"时装：平时零钩子、零延迟
        BOOL needHook = g_opt.mouseRelay && (g_swallow != 0 || g_on || g_opt.relayForce);
        EnsureHook(needHook);

        // 2) 看门狗
        if (g_on && g_fail >= 30) Disarm("连续设置光标失败，先停手（避免吞掉真实鼠标）");
        if (g_on && !DesktopIsOurs()) Disarm("输入桌面不在我们这边（注入会打到别的桌面上）");

        // 3) 广播结束就收手：目标窗口没了再持续 2 个周期 -> 回到零痕迹状态
        if (g_on) {
            if (HasBroadcastWindow()) noBcast = 0;
            else if (++noBcast >= 2) { Disarm("广播窗口已消失，恢复正常输入"); noBcast = 0; }
        } else {
            noBcast = 0;
        }

        // 4) 未接管时探测（接管中不探测：自己的钩子会把探测包也丢掉，只会得到假结论）
        if (!g_on && g_opt.mouseRelay && g_ready) {
            DWORD every = g_opt.relayForce ? 2000 : 5000;
            if (now - lastProbe >= every) {
                lastProbe = now;
                if (!DesktopIsOurs()) {
                    // 桌面被抢时探测没意义（注入打到别的桌面上）
                } else {
                    BOOL sw = ProbeSwallowing();
                    if (g_opt.relayForce) sw = TRUE;
                    if (sw != (g_swallow != 0)) {
                        InterlockedExchange(&g_swallow, sw ? 1 : 0);
                        Log("relay: 探测结果 -> 客户端%s吞鼠标事件", sw ? "正在" : "没有");
                    }
                    if (sw) Arm(1);
                }
            }
        }

        if (++ticks % 10 == 0) LogStats("5s");   // 5 秒一条（本线程不装钩子，写盘不阻塞输入）
        if (g_opt.once) break;
        Sleep(500);
    }
    LogStats("exit");
    return 0;
}

// ---------------------------------------------------------------- 对外
void RelayStart(void)
{
    if (g_started) return;
    g_started = TRUE;
    InterlockedExchange(&g_stop, 0);
    InterlockedExchange(&g_on, 0);
    InterlockedExchange(&g_ready, 0);
    InterlockedExchange(&g_absolute, 0);
    InterlockedExchange(&g_swallow, 0);
    g_rawPkts = g_pktsIgnored = g_relayed = g_buttons = g_hookSeen = 0;
    g_clamped = g_gain2 = g_gain4 = 0;
    g_vvalid = 0;
    g_hookInstalled = CreateEventW(NULL, TRUE, FALSE, NULL);

    // raw sink 一启动就装（只收包，不注入：接管未开始时完全透明）
    g_sinkThread = CreateThread(NULL, 0, SinkThreadProc, NULL, 0, NULL);
    g_monThread  = CreateThread(NULL, 0, MonThreadProc, NULL, 0, NULL);
    Log("relay: starting（直驱光标；不碰客户端内存/驱动/文件）");
}

void RelayStop(void)
{
    if (!g_started) return;
    LogStats("stop");
    InterlockedExchange(&g_on, 0);
    InterlockedExchange(&g_stop, 1);
    if (g_monThread)  { WaitForSingleObject(g_monThread, 3000); CloseHandle(g_monThread); g_monThread = NULL; }
    EnsureHook(FALSE);
    if (g_sinkTid) PostThreadMessageW(g_sinkTid, WM_QUIT, 0, 0);
    if (g_sinkWnd) PostMessageW(g_sinkWnd, WM_CLOSE, 0, 0);
    if (g_sinkThread) { WaitForSingleObject(g_sinkThread, 2000); CloseHandle(g_sinkThread); g_sinkThread = NULL; }
    g_sinkTid = 0;
    if (g_hookInstalled) { CloseHandle(g_hookInstalled); g_hookInstalled = NULL; }
    g_started = FALSE;
    Log("relay: stopped（钩子/隐藏窗口/线程均已回收）");
}

int RelayIsOn(void) { return g_on != 0; }
int RelaySwallowDetected(void) { return g_swallow != 0; }

void RelayGetStats(long* rawPkts, long* injectedPktsIgnored, long* relayed,
                   long* buttons, long* hookSeen, int* on, int* absoluteDevice)
{
    if (rawPkts) *rawPkts = g_rawPkts;
    if (injectedPktsIgnored) *injectedPktsIgnored = g_pktsIgnored;
    if (relayed) *relayed = g_relayed;
    if (buttons) *buttons = g_buttons;
    if (hookSeen) *hookSeen = g_hookSeen;
    if (on) *on = g_on != 0;
    if (absoluteDevice) *absoluteDevice = g_absolute != 0;
}

void RelayGetDiag(long* clamped)
{
    if (clamped) *clamped = g_clamped;
}
