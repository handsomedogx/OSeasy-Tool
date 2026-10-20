// ============================================================================
//  bcast.cpp —— 见 bcast.h 的说明
// ============================================================================
#include "bcast.h"
#include "log.h"
#include "util.h"
#include "config.h"

#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <wchar.h>

extern OPTIONS g_opt;

#define FULLSCREEN_ON_TOKEN   L"#fullscreen#:1"
#define FULLSCREEN_OFF_TOKEN  L"#fullscreen#:0"

static volatile LONG g_stop = 0;
static volatile LONG g_fixCount = 0;
static HANDLE g_thread = NULL;

// 我们自己拉起的渲染器 pid —— 绝不误杀（虽然它的参数里已经没有 fullscreen:1）
#define OWN_SLOTS 8
static DWORD g_own[OWN_SLOTS];
static int   g_ownNext = 0;

static BOOL IsOurs(DWORD pid)
{
    for (int i = 0; i < OWN_SLOTS; i++) if (g_own[i] == pid) return TRUE;
    return FALSE;
}

static void MarkOurs(DWORD pid)
{
    g_own[g_ownNext] = pid;
    g_ownNext = (g_ownNext + 1) % OWN_SLOTS;
}

// ---------------------------------------------------------------- 取别的进程的命令行
// NtQueryInformationProcess(ProcessCommandLineInformation=60)，Win8.1+ 可用。
// 我们以管理员运行，读同用户进程没问题。
#define ProcessCommandLineInformation 60

static BOOL GetRemoteCmdLine(HANDLE hProc, wchar_t* out, int cch)
{
    typedef LONG (NTAPI *PFN)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    static PFN pFn = NULL;
    if (!pFn) {
        HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        if (nt) pFn = (PFN)GetProcAddress(nt, "NtQueryInformationProcess");
        if (!pFn) return FALSE;
    }
    BYTE buf[8192];
    ULONG need = 0;
    memset(buf, 0, sizeof(buf));
    LONG st = pFn(hProc, ProcessCommandLineInformation, buf, sizeof(buf), &need);
    if (st != 0) return FALSE;
    // buf 是 UNICODE_STRING + 紧跟其后的字符串
    USHORT len = *(USHORT*)buf;
    PWSTR  str = *(PWSTR*)(buf + sizeof(ULONG_PTR));
    if (!str || len == 0 || len >= (USHORT)(cch * sizeof(wchar_t))) return FALSE;
    int n = len / sizeof(wchar_t);
    memcpy(out, str, n * sizeof(wchar_t));
    out[n] = 0;
    return TRUE;
}

// ---------------------------------------------------------------- 动作
// 杀 + 用改写后的命令行重新拉起（同一个 exe，同一段参数，只把 fullscreen:1 换成 0）
static BOOL FixOne(DWORD pid, const wchar_t* cmdLine)
{
    // 1) 取 exe 全路径
    HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hp) { Log("bcast: OpenProcess(pid=%lu) 失败 err=%lu", pid, GetLastError()); return FALSE; }
    wchar_t exe[MAX_PATH] = L"";
    DWORD cch = MAX_PATH;
    BOOL gotExe = QueryFullProcessImageNameW(hp, 0, exe, &cch);
    CloseHandle(hp);
    if (!gotExe) { Log("bcast: 取 pid=%lu 路径失败 err=%lu", pid, GetLastError()); return FALSE; }

    // 2) 改写命令行（两个字面量等长，直接原地替换，不破坏引号）
    wchar_t newCmd[8192];
    wcsncpy_s(newCmd, 8192, cmdLine, _TRUNCATE);
    wchar_t* hit = wcsstr(newCmd, FULLSCREEN_ON_TOKEN);
    if (!hit) return FALSE;
    // 等长替换，安全
    memcpy(hit, FULLSCREEN_OFF_TOKEN, wcslen(FULLSCREEN_OFF_TOKEN) * sizeof(wchar_t));

    // 3) 杀掉全屏那个实例
    HANDLE hk = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!hk) { Log("bcast: 无法结束 pid=%lu err=%lu（受保护进程？）", pid, GetLastError()); return FALSE; }
    BOOL killed = TerminateProcess(hk, 0);
    CloseHandle(hk);
    if (!killed) { Log("bcast: TerminateProcess(pid=%lu) 失败 err=%lu", pid, GetLastError()); }
    Sleep(200);   // 等它释放组播/窗口

    // 4) 用窗口广播参数重新拉起（继承我们自己的桌面 -> 画面直接出现在你自己的桌面上）
    STARTUPINFOW si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si)); memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    if (!CreateProcessW(exe, newCmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        Log("bcast: 重新拉起失败 err=%lu  exe=%ls", GetLastError(), exe);
        return FALSE;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    MarkOurs(pi.dwProcessId);
    InterlockedIncrement(&g_fixCount);
    Log("bcast: 全屏广播已改写为窗口广播（原 pid=%lu -> 新 pid=%lu）", pid, pi.dwProcessId);
    return TRUE;
}

// ---------------------------------------------------------------- 监视线程
static DWORD WINAPI BcastProc(LPVOID)
{
    Log("bcast: monitor on（检测 ScreenRender.exe 的 #fullscreen#:1 -> 改写成 #fullscreen#:0%s）",
        g_opt.bcastFix ? "" : "；当前**未启用**改写，只报告");

    DWORD lastAction = 0;
    int   actionsInMinute = 0, minuteTick = 0;
    int   reportedStop = 0;

    while (!g_stop) {
        DWORD now = GetTickCount();
        if (now - lastAction > 60000) actionsInMinute = 0;      // 一分钟滑窗

        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        int targets = 0;
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe; memset(&pe, 0, sizeof(pe)); pe.dwSize = sizeof(pe);
            if (Process32FirstW(snap, &pe)) {
                do {
                    if (_wcsicmp(pe.szExeFile, L"ScreenRender.exe") != 0) continue;
                    if (IsOurs(pe.th32ProcessID)) continue;
                    HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
                    if (!hp) continue;
                    wchar_t cmd[8192];
                    BOOL ok = GetRemoteCmdLine(hp, cmd, 8192);
                    CloseHandle(hp);
                    if (!ok) continue;
                    if (wcsstr(cmd, FULLSCREEN_ON_TOKEN) == NULL) continue;   // 窗口广播，不动它

                    targets++;
                    Log("bcast: 发现全屏广播实例 pid=%lu -> %ls", pe.th32ProcessID, FULLSCREEN_ON_TOKEN);
                    if (!g_opt.bcastFix) continue;
                    if (actionsInMinute >= 8) {                            // 防抖：一分钟最多 8 次
                        if (!reportedStop) { reportedStop = 1; Log("bcast: 一分钟内改写已达 8 次，暂停（客户端可能在抢）"); }
                        continue;
                    }
                    if (!FixOne(pe.th32ProcessID, cmd)) continue;
                    actionsInMinute++; lastAction = GetTickCount(); reportedStop = 0;
                    Sleep(600);                                            // 给它一点启动时间
                } while (Process32NextW(snap, &pe));
            }
            CloseHandle(snap);
        }
        if (minuteTick++ % 60 == 0 && targets == 0) {
            static int told = 0;
            if (told++ < 2) Log("bcast: 未发现全屏广播实例（窗口广播/未广播都不需要动作）");
        }
        if (g_opt.once) break;
        Sleep(1000);
    }
    return 0;
}

// ---------------------------------------------------------------- 对外
void BcastStart(void)
{
    if (g_thread) return;
    InterlockedExchange(&g_stop, 0);
    memset(g_own, 0, sizeof(g_own));
    g_ownNext = 0;
    g_thread = CreateThread(NULL, 0, BcastProc, NULL, 0, NULL);
}

void BcastStop(void)
{
    InterlockedExchange(&g_stop, 1);
    if (g_thread) {
        if (WaitForSingleObject(g_thread, 3000) != WAIT_OBJECT_0)
            Log("worker 'bcast' did not exit in time (reaped on process exit)");
        CloseHandle(g_thread);
        g_thread = NULL;
    }
}

int BcastFixCount(void) { return g_fixCount; }
