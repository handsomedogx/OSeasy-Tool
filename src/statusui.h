// ============================================================================
//  statusui.h — 状态面板（简单 GUI：状态文本 + 四个按钮 + 两个开关）
// ============================================================================
#pragma once
#include <windows.h>

// 由 main.cpp 实现：面板上的"退出程序"按钮走这里（走正常退出路径，不是 TerminateProcess）
void AppRequestExit(void);
// 由 main.cpp 实现：实际生效的热键说明（裸 F8/F9 抢不到会退回 Ctrl+Alt，面板要显示真实情况）
const wchar_t* AppHotkeyDesc(void);

void StatusUiShow(void);     // 没建就建，已有就前置
void StatusUiClose(void);    // 退出程序时调用
void StatusUiBuildText(wchar_t* out, int cch);   // 状态文本（面板 / --status / 弹框共用这一份）
