// ============================================================================
//  desktop.h — 目标②的前提：输入桌面被切走时把输入夺回自己的桌面
// ============================================================================
#pragma once
#include <windows.h>

DWORD WINAPI DesktopProc(LPVOID);       // 后台线程：检测 + 夺回
BOOL ReclaimOwnDesktop(void);           // 立刻夺回一次
BOOL SwitchToClassDesktop(void);        // 切到教学桌面（Ctrl+Alt+F8 用）
int  ReclaimCount(void);
void DesktopHoldReclaim(DWORD ms);      // 用户手动切桌面后，暂停自动夺回（否则点一下就被拉回）
