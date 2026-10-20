// ============================================================================
//  log.cpp
// ============================================================================
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include "log.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>

static FILE*         g_log = NULL;
static CRITICAL_SECTION g_cs;
static BOOL          g_csInit = FALSE;
static BOOL          g_haveConsole = FALSE;
static wchar_t       g_path[MAX_PATH] = L"";

const wchar_t* LogFilePath(void) { return g_path; }

void LogInit(const wchar_t* path)
{
    if (!g_csInit) { InitializeCriticalSection(&g_cs); g_csInit = TRUE; }
    wchar_t buf[MAX_PATH];
    if (path && path[0]) {
        wcsncpy_s(buf, MAX_PATH, path, _TRUNCATE);
    } else {
        GetModuleFileNameW(NULL, buf, MAX_PATH);
        wchar_t* slash = wcsrchr(buf, L'\\');
        if (slash) *(slash + 1) = 0; else buf[0] = 0;
        wcscat_s(buf, MAX_PATH, L"OeWinFix2.log");
    }
    g_log = _wfopen(buf, L"a, ccs=UTF-8");
    wcsncpy_s(g_path, MAX_PATH, buf, _TRUNCATE);

    // 只有真的挂了控制台才往 stdout 写(否则 GUI 进程会白等管道)
    g_haveConsole = (GetConsoleWindow() != NULL);
}

void LogClose(void)
{
    EnterCriticalSection(&g_cs);
    if (g_log) { fclose(g_log); g_log = NULL; }
    LeaveCriticalSection(&g_cs);
}

void Log(const char* fmt, ...)
{
    if (!g_csInit) { InitializeCriticalSection(&g_cs); g_csInit = TRUE; }   // LogInit 之前也可能被调用
    char body[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME st; GetLocalTime(&st);
    EnterCriticalSection(&g_cs);
    if (g_haveConsole) {
        printf("[%02u:%02u:%02u] %s\n", st.wHour, st.wMinute, st.wSecond, body);
        fflush(stdout);
    }
    if (g_log) {
        wchar_t wbody[1200];
        MultiByteToWideChar(CP_UTF8, 0, body, -1, wbody, 1200);
        fwprintf(g_log, L"[%04u-%02u-%02u %02u:%02u:%02u] %ls\n",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, wbody);
        fflush(g_log);
    }
    LeaveCriticalSection(&g_cs);
}

void LogW(const wchar_t* text)
{
    if (!text) return;
    if (!g_csInit) { InitializeCriticalSection(&g_cs); g_csInit = TRUE; }
    EnterCriticalSection(&g_cs);
    if (g_log) { fwprintf(g_log, L"%ls\n", text); fflush(g_log); }
    if (g_haveConsole) { wprintf(L"%ls\n", text); fflush(stdout); }
    LeaveCriticalSection(&g_cs);
}
