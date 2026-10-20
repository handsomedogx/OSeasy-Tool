// ============================================================================
//  util.h/.cpp — 小工具：权限 / 桌面名 / 进程名 / 单实例 / 线程注册
// ============================================================================
#pragma once
#include <windows.h>

BOOL  IsElevated(void);
void  EnablePrivilege(const wchar_t* name);          // 忽略失败(不致命)
BOOL  IsInteractiveSession(void);

// 桌面
BOOL  GetDeskName(HDESK h, wchar_t* out, size_t cch);
HDESK OpenDeskByName(const wchar_t* name, DWORD access);
const wchar_t* OwnDesktopName(void);                 // 启动时记录一次
const wchar_t* InputDesktopName(void);
BOOL  DesktopIsOurs(void);

// 进程
void  GetProcImageName(DWORD pid, wchar_t* out, size_t cch);   // 仅文件名
void  GetProcImagePath(DWORD pid, wchar_t* out, size_t cch);   // 完整路径
BOOL  IsUnderAppDir(const wchar_t* fullPath);

// 单实例：返回 FALSE 表示已有实例在跑(并会把已有窗口叫到前台)
BOOL  AcquireSingleInstance(void);

// 线程注册表：统一 stop 标志 + 优雅退出
typedef DWORD (WINAPI *ThreadProc)(LPVOID);
BOOL  StartWorker(ThreadProc proc, LPVOID param, const char* name);
void  SetStop(void);
BOOL  Stopping(void);
void  StopAllWorkers(DWORD timeoutMs);
