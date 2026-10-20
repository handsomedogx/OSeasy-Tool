// ============================================================================
//  windowize.cpp — 目标①：全屏广播窗口 -> 普通窗口
//
//  为什么这么写（都是实测换来的）：
//   * 判据用窗口类 BCAST_WINDOW_CLASS：真实客户端与替身都用它，比进程名可靠；
//     另加一条兜底"教学系统目录下、覆盖整屏的可见窗口"（防换版本改类名）。
//   * SetWindowPos 必须带 SWP_NOSENDCHANGING：否则客户端在 WM_WINDOWPOSCHANGING 里
//     把尺寸顶回全屏（实测：返回成功但尺寸被改回 1920x1080）。
//   * 客户端会在广播期间反复重设位置/置顶 -> 必须周期纠正，而不是改一次就完事。
//   * 最小化窗口的 rect 是 (-32000,-32000,...)：不能当成"漂移到屏幕外"去摆位，
//     否则每秒空转 5 次永远不收敛（旧项目实测踩过）。
//   * "改了但样式/位置一模一样"（窗口拒绝被摆位）-> 静默 5 秒，避免抖动刷日志。
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#include "windowize.h"
#include "config.h"
#include "facts.h"
#include "log.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>

#define MAX_CAND    32
#define QUIET_SLOTS 16

typedef struct {
    HWND  hwnd;
    DWORD pid;
    wchar_t img[MAX_PATH];
    LONG_PTR style, ex;
    RECT  rc, mon;
    double ratio;
    BOOL  iconic;
} CAND;

static struct { HWND hwnd; DWORD until; } g_quiet[QUIET_SLOTS];
static int g_quietNext = 0;
static int g_fixCount = 0;
static BOOL g_manual = FALSE;   // 手动窗口化(托盘/F9/--windowize-now) -> 允许改尺寸位置

// ---------------------------------------------------------------- 桌面/矩形
static void MonitorOf(HWND hwnd, RECT* mon)
{
    memset(mon, 0, sizeof(*mon));
    HMONITOR mh = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi; mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mh, &mi)) *mon = mi.rcWork;
    else { mon->right = GetSystemMetrics(SM_CXSCREEN); mon->bottom = GetSystemMetrics(SM_CYSCREEN); }
}

static double Coverage(HWND hwnd, RECT* rcOut, RECT* monOut)
{
    RECT rc, mon;
    GetWindowRect(hwnd, &rc);
    MonitorOf(hwnd, &mon);
    double mw = (double)(mon.right - mon.left), mh = (double)(mon.bottom - mon.top);
    double w = (double)(rc.right - rc.left), h = (double)(rc.bottom - rc.top);
    double rw = mw > 0 ? w / mw : 0, rh = mh > 0 ? h / mh : 0;
    if (rcOut) *rcOut = rc;
    if (monOut) *monOut = mon;
    return rw < rh ? rw : rh;
}

static void TargetRect(const RECT* mon, int* x, int* y, int* w, int* h)
{
    if (g_opt.winW > 0 && g_opt.winH > 0) {
        *w = g_opt.winW; *h = g_opt.winH;
        *x = (g_opt.winX != 0) ? g_opt.winX : (mon->left + ((mon->right - mon->left) - *w) / 2);
        *y = (g_opt.winY != 0) ? g_opt.winY : (mon->top + ((mon->bottom - mon->top) - *h) / 2);
    } else {
        *w = (mon->right - mon->left) * WIN_SCALE_NUM / WIN_SCALE_DEN;
        *h = (mon->bottom - mon->top) * WIN_SCALE_NUM / WIN_SCALE_DEN;
        *x = mon->left + ((mon->right - mon->left) - *w) / 2;
        *y = mon->top + ((mon->bottom - mon->top) - *h) / 2;
    }
    if (*w < 320) *w = 320;
    if (*h < 240) *h = 240;
}

// ---------------------------------------------------------------- 候选收集
static BOOL CALLBACK EnumCand(HWND hwnd, LPARAM lp)
{
    struct CTX { CAND* arr; int n; int max; }* c = (struct CTX*)lp;
    if (c->n >= c->max) return FALSE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != NULL) return TRUE;   // 只看顶层

    wchar_t cls[128] = {0};
    GetClassNameW(hwnd, cls, 128);

    BOOL byClass = (_wcsicmp(cls, BCAST_WINDOW_CLASS) == 0);
    RECT rc, mon;
    double ratio = Coverage(hwnd, &rc, &mon);

    BOOL byAppDir = FALSE;
    if (!byClass) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        wchar_t full[MAX_PATH] = L"";
        GetProcImagePath(pid, full, MAX_PATH);
        if (IsUnderAppDir(full) && ratio >= 0.90) byAppDir = TRUE;   // 兜底：换版本改类名时
    }
    if (!byClass && !byAppDir) return TRUE;

    CAND* w = &c->arr[c->n++];
    memset(w, 0, sizeof(*w));
    w->hwnd = hwnd;
    GetWindowThreadProcessId(hwnd, &w->pid);
    GetProcImageName(w->pid, w->img, MAX_PATH);
    w->style = GetWindowLongW(hwnd, GWL_STYLE);
    w->ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    w->rc = rc; w->mon = mon; w->ratio = ratio;
    w->iconic = IsIconic(hwnd) ? TRUE : FALSE;
    return TRUE;
}

static int Collect(CAND* out, int max)
{
    struct CTX { CAND* arr; int n; int max; } ctx = { out, 0, max };
    EnumWindows(EnumCand, (LPARAM)&ctx);
    return ctx.n;
}

// ---------------------------------------------------------------- 纠正一个窗口
static BOOL QuietBlocked(HWND hwnd)
{
    DWORD now = GetTickCount();
    for (int i = 0; i < QUIET_SLOTS; i++)
        if (g_quiet[i].hwnd == hwnd && (LONG)(g_quiet[i].until - now) > 0) return TRUE;
    return FALSE;
}

static void QuietAdd(HWND hwnd, DWORD ms)
{
    g_quiet[g_quietNext].hwnd = hwnd;
    g_quiet[g_quietNext].until = GetTickCount() + ms;
    g_quietNext = (g_quietNext + 1) % QUIET_SLOTS;
}

// "这个窗口我已经给它做过一次几何复位了么" —— 做过之后位置就归用户
#define SEEN_SLOTS 32
static struct { HWND hwnd; } g_seen[SEEN_SLOTS];
static int g_seenNext = 0;

static BOOL SeenBefore(HWND hwnd)
{
    for (int i = 0; i < SEEN_SLOTS; i++) if (g_seen[i].hwnd == hwnd) return TRUE;
    return FALSE;
}

static void MarkSeen(HWND hwnd)
{
    if (SeenBefore(hwnd)) return;
    g_seen[g_seenNext].hwnd = hwnd;
    g_seenNext = (g_seenNext + 1) % SEEN_SLOTS;
}

// "用户摆好的位置"：只在窗口完全落在工作区内时记录；窗口被扔到屏幕外时放回这里
// （而不是永远放回屏幕中间 —— 那样用户会觉得"移动没用"）
#define GOOD_SLOTS 16
static struct { HWND hwnd; RECT rc; BOOL valid; } g_good[GOOD_SLOTS];
static int g_goodNext = 0;

static void UpdateGoodRect(HWND hwnd, const RECT* rc, const RECT* mon)
{
    if (rc->left < mon->left || rc->top < mon->top || rc->right > mon->right || rc->bottom > mon->bottom)
        return;   // 不是"完全在屏幕内"就不记
    for (int i = 0; i < GOOD_SLOTS; i++) {
        if (g_good[i].valid && g_good[i].hwnd == hwnd) { g_good[i].rc = *rc; return; }
    }
    g_good[g_goodNext].hwnd = hwnd;
    g_good[g_goodNext].rc = *rc;
    g_good[g_goodNext].valid = TRUE;
    g_goodNext = (g_goodNext + 1) % GOOD_SLOTS;
}

static BOOL GetGoodRect(HWND hwnd, RECT* out)
{
    for (int i = 0; i < GOOD_SLOTS; i++)
        if (g_good[i].valid && g_good[i].hwnd == hwnd) { *out = g_good[i].rc; return TRUE; }
    return FALSE;
}

static BOOL FixOne(const CAND* c)
{
    LONG_PTR style = c->style, ex = c->ex;
    BOOL need = FALSE;

    if (ex & WS_EX_TOPMOST)     { ex &= ~WS_EX_TOPMOST;   need = TRUE; }
    if (style & WS_POPUP)       { style &= ~WS_POPUP;     need = TRUE; }
    if ((style & WS_CAPTION) != WS_CAPTION) {
        style |= WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX;
        need = TRUE;
    }
    if (ex & WS_EX_TRANSPARENT) { ex &= ~WS_EX_TRANSPARENT; need = TRUE; }

    int x, y, w, h;
    TargetRect(&c->mon, &x, &y, &w, &h);

    // 关键设计（用户反馈换来的）：
    //   * 客户端广播期间会反复给窗口重加 TOPMOST/WS_POPUP -> 那时**只改样式，不碰位置**，
    //     否则用户拖走 200ms 就被拉回屏幕中间；
    //   * 位置只在这两种情况下才复位：① 真的全屏(覆盖>=90%)  ② **首次**发现它大面积在屏幕外；
    //     之后位置归用户，客户端再怎么挪我们也不抢（不再把"稍微出界"当成漂移）。
    BOOL needGeo = FALSE;
    BOOL restoreGood = FALSE;
    // 守护默认**不碰位置尺寸**（noResize=1）：全屏广播已由 bcast 模块从源头改成窗口广播；
    // 只有手动窗口化(托盘/F9)才允许改几何。
    if ((!g_opt.noResize || g_manual) && !c->iconic) {
        BOOL fullscreen = (c->ratio >= 0.90);
        BOOL mostlyOut  = TRUE;
        {
            RECT inter;
            BOOL has = IntersectRect(&inter, &c->rc, &c->mon);
            double winArea = (double)(c->rc.right - c->rc.left) * (c->rc.bottom - c->rc.top);
            double visArea = has ? (double)(inter.right - inter.left) * (inter.bottom - inter.top) : 0.0;
            mostlyOut = (winArea <= 0) || ((visArea / winArea) < 0.5);
        }
        if (g_manual) {
            // 手动"立即窗口化"：**一定摆到位**（默认工作区 80% 居中）。
            // 以前这里也走"只在全屏/越界时才动"的克制逻辑 —— 结果 B 方案把窗口改好之后，
            // 用户点这个按钮**没有任何可见变化**，看起来就是"这个功能没用"。
            needGeo = TRUE;
        } else if (fullscreen) {
            // 全屏 -> 窗口化。位置**优先回到"用户上次摆好的位置"**（不再一律居中）,
            // 只有还没记住任何位置时才用默认的"工作区 80% 居中"。
            RECT good;
            if (GetGoodRect(c->hwnd, &good) &&
                (good.right - good.left) > 240 && (good.bottom - good.top) > 180) {
                x = good.left; y = good.top;
                w = good.right - good.left; h = good.bottom - good.top;
                restoreGood = TRUE;
            }
            needGeo = TRUE;
        } else if (mostlyOut) {
            // 大半在屏幕外：要么是刚出现，要么是客户端把它扔出去了 -> 放回"用户上次摆好的位置"
            RECT good;
            if (GetGoodRect(c->hwnd, &good)) {
                x = good.left; y = good.top;
                w = good.right - good.left; h = good.bottom - good.top;
                restoreGood = TRUE;
            }
            needGeo = TRUE;
        }
        // 记住"用户摆好的位置"：完全在屏幕内、且**不是全屏尺寸**时才记，
        // 否则会把"全屏矩形"记成好位置，下次复位又变回全屏（死循环）。
        if (!fullscreen) UpdateGoodRect(c->hwnd, &c->rc, &c->mon);
    }
    if (!need && !needGeo) return FALSE;
    if (QuietBlocked(c->hwnd)) return FALSE;

    SetWindowLongW(c->hwnd, GWL_STYLE, style);
    SetWindowLongW(c->hwnd, GWL_EXSTYLE, ex);
    if (needGeo) {
        MarkSeen(c->hwnd);
        if (restoreGood) QuietAdd(c->hwnd, 3000);   // 救回来之后 3 秒内不跟客户端拉扯
        SetWindowPos(c->hwnd, HWND_NOTOPMOST, x, y, w, h,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE |
                     SWP_NOOWNERZORDER | SWP_NOSENDCHANGING_BIT);
    } else {
        // 只修样式: 绝不碰位置尺寸(用户自己摆好的位置要留住)
        SetWindowPos(c->hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | SWP_NOACTIVATE |
                     SWP_NOOWNERZORDER | SWP_NOSENDCHANGING_BIT);
    }
    if (ex & WS_EX_LAYERED) SetLayeredWindowAttributes(c->hwnd, 0, 255, LWA_ALPHA);

    RECT now = {0};
    GetWindowRect(c->hwnd, &now);
    LONG_PTR ns = GetWindowLongW(c->hwnd, GWL_STYLE);
    LONG_PTR nx = GetWindowLongW(c->hwnd, GWL_EXSTYLE);
    BOOL noop = (ns == c->style && nx == c->ex &&
                 now.left == c->rc.left && now.top == c->rc.top &&
                 now.right == c->rc.right && now.bottom == c->rc.bottom);
    if (noop) {
        QuietAdd(c->hwnd, 5000);
        static int noopLogged = 0;
        if (noopLogged++ < 5)
            Log("windowize: 摆了但没变化(hwnd=0x%08X img=%ls iconic=%d) -> 静默 5s",
                (unsigned)(ULONG_PTR)c->hwnd, c->img, (int)c->iconic);
        return FALSE;
    }
    static int logged = 0;
    if (g_opt.verbose || logged < 40) {
        Log("windowize: %s hwnd=0x%08X img=%ls ratio=%.2f iconic=%d style 0x%08X->0x%08X ex 0x%08X->0x%08X rect (%d,%d,%d,%d)->(%d,%d,%d,%d)",
            needGeo ? "复位几何(全屏/越界/过大)" : "只修样式(位置不动)",
            (unsigned)(ULONG_PTR)c->hwnd, c->img, c->ratio, (int)c->iconic,
            (unsigned)c->style, (unsigned)ns, (unsigned)c->ex, (unsigned)nx,
            (int)c->rc.left, (int)c->rc.top, (int)c->rc.right, (int)c->rc.bottom,
            (int)now.left, (int)now.top, (int)now.right, (int)now.bottom);
        logged++;
    }
    return TRUE;
}

// ---------------------------------------------------------------- 对外接口
int WindowizeNowManual(void)
{
    g_manual = TRUE;
    int n = WindowizeNow();
    g_manual = FALSE;
    return n;
}

int WindowizeNow(void)
{
    CAND list[MAX_CAND];
    int n = Collect(list, MAX_CAND), fixed = 0;
    for (int i = 0; i < n; i++) if (FixOne(&list[i])) fixed++;
    if (fixed) g_fixCount += fixed;
    return fixed;
}

// ---------------------------------------------------------------- 看大画面（最大化/还原）
// 用户要的"想学时看一眼全屏"：不切桌面（B 方案下教学桌面是空的），
// 直接把广播窗口在**工作区内**铺满；再按一次回到原来的位置尺寸。
static struct { HWND hwnd; RECT prev; BOOL active; LONG_PTR prevStyle, prevEx; } g_max;

int WindowizeToggleMaximize(void)
{
    CAND list[MAX_CAND];
    int n = Collect(list, MAX_CAND);
    if (n <= 0) return 0;

    const CAND* c = &list[0];
    RECT mon = c->mon;                              // rcWork：不盖住任务栏

    BOOL goingMax = !(g_max.active && g_max.hwnd == c->hwnd);

    // 样式先摆正，免得又是个 hwnd 带 POPUP/TOPMOST 的怪窗口
    LONG_PTR style = c->style, ex = c->ex;
    if (!goingMax) { style = g_max.prevStyle; ex = g_max.prevEx; }
    else           { g_max.prevStyle = c->style; g_max.prevEx = c->ex; }

    if (goingMax) {
        // 抓"铺满前"的矩形，但先校验：客户端会周期性重设自己的几何，
        // 实测抓到过 1440x1100（比 1080 的屏幕还高），照着还原就会还原到屏幕外。
        RECT prev = c->rc;
        int pw = (int)(prev.right - prev.left), ph = (int)(prev.bottom - prev.top);
        int mw = (int)(mon.right - mon.left), mh = (int)(mon.bottom - mon.top);
        if (pw < 320 || ph < 240 || pw > mw || ph > mh) {
            int x, y, w, h;
            TargetRect(&mon, &x, &y, &w, &h);
            prev.left = x; prev.top = y; prev.right = x + w; prev.bottom = y + h;
            Log("windowize: 铺满前的窗口尺寸不正常（%dx%d）-> 还原目标改用默认 %dx%d", pw, ph, w, h);
        }
        g_max.prev = prev;
        g_max.hwnd = c->hwnd;
        g_max.active = TRUE;
        style &= ~WS_POPUP;
        ex    &= ~WS_EX_TOPMOST;
        style |= WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
        SetWindowLongW(c->hwnd, GWL_STYLE, style);
        SetWindowLongW(c->hwnd, GWL_EXSTYLE, ex);
        // 要"提到最前，但不要持久置顶"：先 HWND_TOPMOST 再立刻 HWND_NOTOPMOST。
        // 单纯 HWND_TOP 在高 DPI / 客户端周期性重设几何时不一定生效（实测 z 序会被顶下去）。
        // 这一下来完，窗口在最前，但 ex 里没有 WS_EX_TOPMOST —— 之后你切到别的程序它照样会被盖住。
        SetWindowPos(c->hwnd, HWND_TOPMOST, mon.left, mon.top, mon.right - mon.left, mon.bottom - mon.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE |
                     SWP_NOOWNERZORDER | SWP_NOSENDCHANGING_BIT);
        SetWindowPos(c->hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        BringWindowToTop(c->hwnd);
        SetForegroundWindow(c->hwnd);      // 允许失败：渲染窗口可能不接受激活
        Log("windowize: 看大画面 -> 铺满工作区 (%d,%d) %dx%d（已在最前，未置顶；再按一次还原）",
            (int)mon.left, (int)mon.top, (int)(mon.right - mon.left), (int)(mon.bottom - mon.top));
    } else {
        RECT back = g_max.prev;
        int bw = (int)(back.right - back.left), bh = (int)(back.bottom - back.top);
        int mw = (int)(mon.right - mon.left), mh = (int)(mon.bottom - mon.top);
        // 兜底：还原矩形必须落在工作区内且尺寸合理（见上面那次实测踩坑）
        BOOL sane = (bw >= 320 && bh >= 240 && bw <= mw && bh <= mh);
        if (!sane) {
            int x, y, w, h;
            TargetRect(&mon, &x, &y, &w, &h);
            back.left = x; back.top = y; back.right = x + w; back.bottom = y + h;
            Log("windowize: 记住的矩形不可用（%dx%d）-> 还原成默认 %dx%d", bw, bh, w, h);
        }
        g_max.active = FALSE;
        SetWindowLongW(c->hwnd, GWL_STYLE, style);
        SetWindowLongW(c->hwnd, GWL_EXSTYLE, ex);
        SetWindowPos(c->hwnd, HWND_NOTOPMOST, back.left, back.top,
                     back.right - back.left, back.bottom - back.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE |
                     SWP_NOOWNERZORDER | SWP_NOSENDCHANGING_BIT);
        Log("windowize: 看大画面 -> 还原到 (%d,%d) %dx%d",
            (int)back.left, (int)back.top, (int)(back.right - back.left), (int)(back.bottom - back.top));
    }
    g_fixCount++;
    return 1;
}

int WindowizeFixCount(void) { return g_fixCount; }

// 跨桌面：广播窗口在"教学桌面"上时，需要把纠正线程挂到那个桌面去做
BOOL WindowizeOnDesktop(const wchar_t* deskName)
{
    if (!deskName || !deskName[0]) return FALSE;
    if (_wcsicmp(deskName, OwnDesktopName()) == 0) { WindowizeNow(); return TRUE; }

    HDESK d = OpenDeskByName(deskName, DESKTOP_READOBJECTS | DESKTOP_ENUMERATE |
                                       DESKTOP_WRITEOBJECTS | DESKTOP_SWITCHDESKTOP);
    if (!d) { Log("windowize: OpenDesktop('%ls') failed err=%lu", deskName, GetLastError()); return FALSE; }

    // ARG 必须放堆上: 线程如果 10 秒没干完，主线程不能再回收栈对象/关掉桌面句柄
    // （旧写法把栈地址交给线程，超时后 CloseDesktop 会让那个线程踩空）。
    struct ARG { HDESK desk; int fixed; };
    ARG* a = (ARG*)malloc(sizeof(ARG));
    if (!a) { CloseDesktop(d); return FALSE; }
    a->desk = d; a->fixed = 0;
    HANDLE t = CreateThread(NULL, 0, [](LPVOID p) -> DWORD {
        ARG* a = (ARG*)p;
        if (!SetThreadDesktop(a->desk)) {          // 必须第一步就切桌面（本线程此前无窗口/钩子）
            Log("windowize: SetThreadDesktop failed err=%lu", GetLastError());
            return 0;
        }
        CAND list[MAX_CAND];
        int n = Collect(list, MAX_CAND);
        for (int i = 0; i < n; i++) if (FixOne(&list[i])) a->fixed++;
        return 0;
    }, a, 0, NULL);
    if (!t) { Log("windowize: 跨桌面线程创建失败 err=%lu", GetLastError()); free(a); CloseDesktop(d); return FALSE; }

    int fixedCount = 0;
    if (WaitForSingleObject(t, 10000) == WAIT_OBJECT_0) {
        fixedCount = a->fixed;
        free(a);
        CloseDesktop(d);
    } else {
        // 线程还活着 -> 桌面句柄和 ARG 都留给他，不回收（宁可泄漏也不制造悬空指针）
        Log("windowize: 跨桌面线程 10 秒未完成 -> 放弃本次（句柄交给线程，不回收）");
        CloseHandle(t);
        return FALSE;
    }
    CloseHandle(t);
    if (fixedCount) Log("windowize: 跨桌面纠正 '%ls' 了 %d 个窗口", deskName, fixedCount);
    return fixedCount > 0;
}

DWORD WINAPI WindowizeProc(LPVOID)
{
    Log("windowize: keeper on (class=%ls, 周期 %dms)", BCAST_WINDOW_CLASS, g_opt.windowIntervalMs);
    do {
        WindowizeNow();
        // 窗口在教学桌面上时，顺手跨桌面纠正一次（best effort）
        if (!HasBroadcastWindow()) {
            HDESK d = OpenDeskByName(TEACH_DESKTOP_NAME, DESKTOP_READOBJECTS | DESKTOP_ENUMERATE);
            if (d) { CloseDesktop(d); WindowizeOnDesktop(TEACH_DESKTOP_NAME); }
        }
        if (g_opt.once) break;
        Sleep(g_opt.windowIntervalMs);
    } while (!Stopping());
    Log("windowize: keeper exit (累计纠正 %d 次)", g_fixCount);
    return 0;
}

// ---------------------------------------------------------------- 广播存在检测
static BOOL CALLBACK FindBcast(HWND h, LPARAM lp)
{
    if (!IsWindowVisible(h)) return TRUE;
    wchar_t cls[128] = {0};
    GetClassNameW(h, cls, 128);
    if (_wcsicmp(cls, BCAST_WINDOW_CLASS) == 0) { *(BOOL*)lp = TRUE; return FALSE; }
    return TRUE;
}

BOOL BroadcastWindowOnOwnDesktop(void)
{
    BOOL found = FALSE;
    EnumDesktopWindows(NULL, FindBcast, (LPARAM)&found);   // 只查当前线程所在桌面
    return found;
}

BOOL HasBroadcastWindow(void)
{
    BOOL found = FALSE;
    EnumDesktopWindows(NULL, FindBcast, (LPARAM)&found);       // 当前线程所在桌面
    if (found) return TRUE;
    const wchar_t* desks[2] = { TEACH_DESKTOP_NAME, TEACH_DESKTOP_NAME2 };
    for (int i = 0; i < 2; i++) {
        HDESK d = OpenDeskByName(desks[i], DESKTOP_READOBJECTS | DESKTOP_ENUMERATE);
        if (!d) continue;
        EnumDesktopWindows(d, FindBcast, (LPARAM)&found);
        CloseDesktop(d);
        if (found) return TRUE;
    }
    return FALSE;
}
