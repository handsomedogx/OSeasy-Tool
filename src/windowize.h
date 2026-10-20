// ============================================================================
//  windowize.h — 目标①：把"全屏广播窗口"变成普通窗口
// ============================================================================
#pragma once
#include <windows.h>

// 是否存在"正在广播"的窗口（任一桌面：当前桌面 / 教学桌面）
BOOL HasBroadcastWindow(void);

// 现在立刻纠正一次（返回被处理的窗口数）
int  WindowizeNow(void);        // 守护周期调用（默认只修样式）
int  WindowizeNowManual(void);  // 手动：允许改尺寸位置（托盘/F9/--windowize-now）

// 后台纠正线程：每 g_opt.windowIntervalMs 检查一次，发现又被铺满/置顶就再掰回来
DWORD WINAPI WindowizeProc(LPVOID);

// 目标桌面名（广播窗口在别的桌面上时用它去做跨桌面纠正；成功返回 TRUE）
BOOL WindowizeOnDesktop(const wchar_t* deskName);

// 累计纠正次数（状态显示用）
int  WindowizeFixCount(void);
