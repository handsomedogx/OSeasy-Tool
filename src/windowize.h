// ============================================================================
//  windowize.h — 目标①：把"全屏广播窗口"变成普通窗口
// ============================================================================
#pragma once
#include <windows.h>

// 是否存在"正在广播"的窗口（任一桌面：当前桌面 / 教学桌面）
BOOL HasBroadcastWindow(void);

// 广播窗口是不是就在**我们自己的桌面**上（B 方案生效后就是这样）
// —— 决定"镜像窗口"和"切到课堂桌面"这两个入口还有没有意义
BOOL BroadcastWindowOnOwnDesktop(void);

// 现在立刻纠正一次（返回被处理的窗口数）
int  WindowizeNow(void);        // 守护周期调用（默认只修样式）
int  WindowizeNowManual(void);  // 手动：允许改尺寸位置（托盘/F9/--windowize-now）

// 看大画面：把广播窗口在工作区内"最大化/还原"来回切（F8 / 面板按钮）
// 不切桌面 —— B 方案已把广播搬到你自己的桌面上，切过去只会看到空桌面。
// 返回处理的窗口数（0 = 当前没有广播窗口）。
int  WindowizeToggleMaximize(void);

// 后台纠正线程：每 g_opt.windowIntervalMs 检查一次，发现又被铺满/置顶就再掰回来
DWORD WINAPI WindowizeProc(LPVOID);

// 目标桌面名（广播窗口在别的桌面上时用它去做跨桌面纠正；成功返回 TRUE）
BOOL WindowizeOnDesktop(const wchar_t* deskName);

// 累计纠正次数（状态显示用）
int  WindowizeFixCount(void);
