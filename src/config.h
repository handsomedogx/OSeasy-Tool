// ============================================================================
//  config.h — 运行选项（CLI 单表解析后填这里；模块只读它）
// ============================================================================
#pragma once

typedef struct _OPTIONS {
    // 功能开关（默认全开）
    int windowize;
    int desktopReclaim;
    int mirror;
    int mouseRelay;          // 默认**开**: 探测驱动 —— 只在证明客户端在吞事件时才接管，
                             // 平时完全透明（零钩子、零注入）。接管方式是直驱光标(SetCursorPos)。
    int mouseAccel;          // 鼠标加速曲线: 默认**关**(线性 1:1, 灵敏度低但可预测)
                             // 开 = 复刻系统"提高指针精确度"(阈值 6/10 -> x2/x4)。托盘可切，存 HKCU。
    int bcastFix;            // 默认开: 发现全屏广播(#fullscreen#:1)就改写成窗口广播(#fullscreen#:0)
                             // —— 从源头绕开"锁键鼠 + 切桌面"，不需要鼠标中继
    int keyboard;
    int tray;

    // 周期
    int windowIntervalMs;    // 窗口化纠正周期（默认 200）
    int desktopIntervalMs;   // 桌面检测/夺回周期（默认 250）
    int kbdIntervalMs;       // 键盘释放周期（默认 400）
    int mirrorFps;           // 镜像帧率（默认 10）

    // 窗口化几何：0 表示"工作区 80% 居中"
    int winW, winH, winX, winY;
    int noResize;            // 只去置顶/全屏样式，不动尺寸位置

    // 诊断
    int verbose;
    int once;                // 各线程只跑一轮（自检用）
    int relayForce;          // 不检测广播也接管鼠标中继（测试用）
    int showStatus;          // --status
    int windowizeNow;        // --windowize-now（做一次就退出）
    int quit;                // --quit：请已在运行的实例退出（不需要找托盘图标）
    int showPanel;           // 启动时弹状态面板（默认 1；--no-panel 关掉）
} OPTIONS;

extern OPTIONS g_opt;
