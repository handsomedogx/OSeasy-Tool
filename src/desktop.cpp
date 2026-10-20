// ============================================================================
//  desktop.cpp — 输入桌面夺回
//
//  实测事实：教师端广播时会把**输入桌面**切到 NetTeachingDesktop。
//  一旦不是自己的桌面，自己的桌面上收不到任何鼠标/键盘输入 —— 这就是
//  "鼠标不能移动"的最底层原因（实测 299 次采样 inputDesktop=NetTeachingDesktop）。
//  夺回只要一个 SwitchDesktop(自己桌面)，实测返回成功、立即生效。
//
//  注：客户端的鼠标钩子是**会话级**的，夺回桌面并不会让它失效 —— 所以还需要鼠标中继。
//      夺回桌面解决"输入归谁"，中继解决"输入被吞"，两件事都要做。
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#include "desktop.h"
#include "config.h"
#include "facts.h"
#include "log.h"
#include "util.h"

static volatile LONG g_reclaims = 0;
static DWORD g_lastLog = 0;

BOOL ReclaimOwnDesktop(void)
{
    const wchar_t* own = OwnDesktopName();
    HDESK d = OpenDeskByName(own, DESKTOP_SWITCHDESKTOP | DESKTOP_READOBJECTS);
    if (!d) { Log("desktop: OpenDesktop('%ls') failed err=%lu", own, GetLastError()); return FALSE; }
    BOOL ok = SwitchDesktop(d);
    CloseDesktop(d);
    if (ok) {
        InterlockedIncrement(&g_reclaims);
        DWORD now = GetTickCount();
        LONG n = InterlockedCompareExchange(&g_reclaims, 0, 0);
        if (n <= 3 || now - g_lastLog > 60000) {      // 日志限流：前 3 次 + 每分钟一次
            Log("desktop: 夺回自己的桌面 '%ls' (第 %ld 次)", own, (long)n);
            g_lastLog = now;
        }
    } else {
        Log("desktop: SwitchDesktop('%ls') failed err=%lu", own, GetLastError());
    }
    return ok;
}

BOOL SwitchToClassDesktop(void)
{
    const wchar_t* names[2] = { TEACH_DESKTOP_NAME, TEACH_DESKTOP_NAME2 };
    for (int i = 0; i < 2; i++) {
        HDESK d = OpenDeskByName(names[i], DESKTOP_SWITCHDESKTOP | DESKTOP_READOBJECTS | DESKTOP_ENUMERATE);
        if (!d) continue;
        BOOL ok = SwitchDesktop(d);
        CloseDesktop(d);
        if (ok) { Log("desktop: 切到课堂桌面 '%ls'", names[i]); return TRUE; }
    }
    Log("desktop: 课堂桌面不存在或无法切换");
    return FALSE;
}

int ReclaimCount(void) { return (int)InterlockedCompareExchange(&g_reclaims, 0, 0); }

DWORD WINAPI DesktopProc(LPVOID)
{
    Log("desktop: guard on (own='%ls', 周期 %dms)", OwnDesktopName(), g_opt.desktopIntervalMs);
    BOOL wasForeign = FALSE;
    do {
        BOOL foreign = !DesktopIsOurs();
        if (foreign) {
            if (!wasForeign) {
                Log("desktop: 输入桌面被切到 '%ls' (自己 '%ls') -> 夺回", InputDesktopName(), OwnDesktopName());
                wasForeign = TRUE;
            }
            if (g_opt.desktopReclaim) ReclaimOwnDesktop();
        } else {
            wasForeign = FALSE;
        }
        if (g_opt.once) break;
        Sleep(g_opt.desktopIntervalMs);
    } while (!Stopping());
    Log("desktop: guard exit (累计夺回 %d 次)", ReclaimCount());
    return 0;
}
