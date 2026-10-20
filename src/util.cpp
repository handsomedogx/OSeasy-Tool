// ============================================================================
//  util.cpp
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#include "util.h"
#include "facts.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- 权限
BOOL IsElevated(void)
{
    BOOL admin = FALSE;
    PSID grp = NULL;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
                                 0, 0, 0, 0, 0, 0, &grp)) {
        CheckTokenMembership(NULL, grp, &admin);
        FreeSid(grp);
    }
    return admin;
}

void EnablePrivilege(const wchar_t* name)
{
    HANDLE tok = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) return;
    TOKEN_PRIVILEGES tp; memset(&tp, 0, sizeof(tp));
    tp.PrivilegeCount = 1;
    if (LookupPrivilegeValueW(NULL, name, &tp.Privileges[0].Luid)) {
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), NULL, NULL);
    }
    CloseHandle(tok);
}

BOOL IsInteractiveSession(void)
{
    DWORD sid = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &sid)) return FALSE;
    return sid != 0;   // session 0 = 服务，不交互
}

// ---------------------------------------------------------------- 桌面
BOOL GetDeskName(HDESK h, wchar_t* out, size_t cch)
{
    if (out && cch) out[0] = 0;
    if (!h) return FALSE;
    DWORD need = 0;
    return GetUserObjectInformationW(h, UOI_NAME, out, (DWORD)(cch * sizeof(wchar_t)), &need) ? TRUE : FALSE;
}

HDESK OpenDeskByName(const wchar_t* name, DWORD access)
{
    return OpenDesktopW(name, 0, FALSE, access);
}

const wchar_t* OwnDesktopName(void)
{
    static wchar_t own[128] = L"";
    if (!own[0]) {
        if (!GetDeskName(GetThreadDesktop(GetCurrentThreadId()), own, 128))
            wcscpy_s(own, 128, L"Default");
    }
    return own;
}

const wchar_t* InputDesktopName(void)
{
    static wchar_t buf[128];
    buf[0] = 0;
    HDESK d = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (d) {
        DWORD need = 0;
        GetUserObjectInformationW(d, UOI_NAME, buf, sizeof(buf), &need);
        CloseDesktop(d);
    }
    if (!buf[0]) wcscpy_s(buf, 128, L"?");
    return buf;
}

BOOL DesktopIsOurs(void)
{
    return _wcsicmp(InputDesktopName(), OwnDesktopName()) == 0;
}

// ---------------------------------------------------------------- 进程
void GetProcImageName(DWORD pid, wchar_t* out, size_t cch)
{
    out[0] = 0;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h) return;
    wchar_t full[MAX_PATH] = L"";
    DWORD n = MAX_PATH;
    if (QueryFullProcessImageNameW(h, 0, full, &n)) {
        const wchar_t* base = wcsrchr(full, L'\\');
        wcsncpy_s(out, cch, base ? base + 1 : full, _TRUNCATE);
    }
    CloseHandle(h);
}

void GetProcImagePath(DWORD pid, wchar_t* out, size_t cch)
{
    out[0] = 0;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h) return;
    DWORD n = (DWORD)cch;
    QueryFullProcessImageNameW(h, 0, out, &n);
    CloseHandle(h);
}

BOOL IsUnderAppDir(const wchar_t* fullPath)
{
    if (!fullPath || !fullPath[0]) return FALSE;
    return _wcsnicmp(fullPath, APP_DIR_DEFAULT, wcslen(APP_DIR_DEFAULT)) == 0;
}

// ---------------------------------------------------------------- 单实例
#define MUTEX_NAME L"Global\\OeWinFix2_SingleInstance"
static HANDLE g_mutex = NULL;

BOOL AcquireSingleInstance(void)
{
    g_mutex = CreateMutexW(NULL, FALSE, MUTEX_NAME);
    if (!g_mutex) return TRUE;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // 把已有实例的窗口叫到前台，然后自己退出
        HWND h = FindWindowW(L"OeWinFix2Status", NULL);
        if (h) { ShowWindow(h, SW_RESTORE); SetForegroundWindow(h); }
        return FALSE;
    }
    return TRUE;
}

// ---------------------------------------------------------------- 线程注册表
#define MAX_WORKERS 12
typedef struct { HANDLE h; char name[32]; } WORKER;
static WORKER g_workers[MAX_WORKERS];
static int    g_nWorkers = 0;
static volatile LONG g_stop = 0;
static CRITICAL_SECTION g_workerCs;
static BOOL   g_workerCsInit = FALSE;

typedef struct { ThreadProc proc; LPVOID param; const char* name; } STARTINFO;

static DWORD WINAPI WorkerTrampoline(LPVOID p)
{
    STARTINFO si = *(STARTINFO*)p;
    free(p);
    DWORD rc = si.proc(NULL);
    (void)rc;
    return 0;
}

BOOL StartWorker(ThreadProc proc, LPVOID param, const char* name)
{
    if (!g_workerCsInit) { InitializeCriticalSection(&g_workerCs); g_workerCsInit = TRUE; }
    STARTINFO* si = (STARTINFO*)malloc(sizeof(STARTINFO));
    if (!si) return FALSE;
    si->proc = proc; si->param = param; si->name = name;
    HANDLE h = CreateThread(NULL, 0, WorkerTrampoline, si, 0, NULL);
    if (!h) { free(si); return FALSE; }
    EnterCriticalSection(&g_workerCs);
    if (g_nWorkers < MAX_WORKERS) {
        g_workers[g_nWorkers].h = h;          // 句柄留给 StopAllWorkers 关（**这里绝不能关**，
        strncpy_s(g_workers[g_nWorkers].name, 32, name ? name : "worker", _TRUNCATE);  // 之前关早了 ->
        g_nWorkers++;                          // WaitForSingleObject 拿到的是失效句柄，
    } else {                                   // 退出时全部误报 "did not exit in time"）
        CloseHandle(h);
    }
    LeaveCriticalSection(&g_workerCs);
    return TRUE;
}

void SetStop(void) { InterlockedExchange(&g_stop, 1); }
BOOL Stopping(void) { return g_stop != 0; }

void StopAllWorkers(DWORD timeoutMs)
{
    SetStop();
    DWORD t0 = GetTickCount();
    EnterCriticalSection(&g_workerCs);
    for (int i = 0; i < g_nWorkers; i++) {
        DWORD left = 0;
        DWORD spent = GetTickCount() - t0;
        left = (spent >= timeoutMs) ? 0 : (timeoutMs - spent);
        DWORD r = WaitForSingleObject(g_workers[i].h, left);
        if (r != WAIT_OBJECT_0) Log("worker '%s' did not exit in time (reaped on process exit)", g_workers[i].name);
        CloseHandle(g_workers[i].h);
    }
    g_nWorkers = 0;
    LeaveCriticalSection(&g_workerCs);
}
