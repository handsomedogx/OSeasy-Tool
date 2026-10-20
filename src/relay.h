// ============================================================================
//  relay.h — 目标②：鼠标（广播期间让真实鼠标可用＝直驱光标）
// ============================================================================
#pragma once
#include <windows.h>

void RelayStart(void);
void RelayStop(void);
int  RelayIsOn(void);
int  RelaySwallowDetected(void);   // 最近一次探测结论：客户端是否在吞鼠标事件

// 加速曲线开关（0 = 线性 1:1，灵敏度低但完全可预测；1 = 复刻系统"提高指针精确度"）
void RelaySetAccel(int on);
int  RelayGetAccel(void);
void RelaySetSens(double factor);   // 灵敏度倍数（0.2 ~ 4.0，默认 1.0 = 设备计数:像素 = 1:1）
double RelayGetSens(void);
void RelayLoadSettings(void);      // 启动时从 HKCU\Software\OeWinFix2 读回
void RelaySaveSettings(void);      // 改动后写入

// 统计（给状态栏/日志用；任一参数可为 NULL）
void RelayGetStats(long* rawPkts, long* injectedPktsIgnored, long* relayed,
                   long* buttons, long* hookSeen, int* on, int* absoluteDevice);
void RelayGetDiag(long* clamped);   // 撞边次数（面板显示用）
