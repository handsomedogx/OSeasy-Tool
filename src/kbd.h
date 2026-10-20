// ============================================================================
//  kbd.h — 目标②：键盘（KbFilter.sys 驱动级封锁的解除）
// ============================================================================
#pragma once
#include <windows.h>

DWORD WINAPI KeyboardProc(LPVOID);   // 后台线程：每 g_opt.kbdIntervalMs 下发一次释放 IOCTL
BOOL KbdReleaseOnce(void);           // 一次性释放（返回是否成功）
BOOL KbdAvailable(void);             // 设备是否可打开
