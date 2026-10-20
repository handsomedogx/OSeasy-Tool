// ============================================================================
//  mirror.h — 目标①的延伸：留在自己桌面的同时，仍然能看到老师的广播画面
// ============================================================================
#pragma once
#include <windows.h>

void MirrorStart(void);       // 启动镜像窗口 + 抓帧线程
void MirrorStop(void);
HWND MirrorWindow(void);
// 用户主动隐藏/显示：隐藏后抓帧循环不再自动弹回来（托盘菜单用它）
void MirrorSetUserHidden(int hidden);
