// ============================================================================
//  relaytest.cpp — 单独验证 src/relay.cpp（不启动主程序、不碰正在跑的 OSeasy-Tool.exe）
//
//  用法: relaytest.exe [--accel]
//    起 relay 线程 8 秒，每 0.5 秒打印一次统计；--relay-force 语义在下面写死：
//    第一次探测就直接接管，方便在没有真实广播时也能验证"接管+直驱光标"这条链。
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <wchar.h>

#include "../src/config.h"
#include "../src/relay.h"
#include "../src/log.h"

// relay.cpp 依赖的外部符号（正式程序里由 util.cpp / windowize.cpp 提供）
BOOL DesktopIsOurs(void)      { return TRUE; }
BOOL HasBroadcastWindow(void) { return TRUE; }

OPTIONS g_opt = {
    1, 1, 1, -1, 1, 1, 1,      // windowize/desktopReclaim/mouseRelay/mouseAccel(-1)/bcastFix/keyboard/tray
    200, 250, 400,             // 周期
    0, 0, 0, 0,                // 窗口几何
    1,                         // noResize
    0, 0, 1, 0, 0              // verbose/once/relayForce=1(不等探测)/showStatus/windowizeNow
};

int wmain(int argc, wchar_t** argv)
{
    for (int i = 1; i < argc; i++)
        if (_wcsicmp(argv[i], L"--accel") == 0) g_opt.mouseAccel = 1;

    LogInit(L"bin\\dev\\relaytest.log");
    RelayLoadSettings();
    if (g_opt.mouseAccel >= 0) RelaySetAccel(g_opt.mouseAccel);
    printf("accel=%d mode=direct\n", RelayGetAccel());

    RelayStart();
    for (int i = 0; i < 16; i++) {
        Sleep(500);
        long raw = 0, ign = 0, rel = 0, btn = 0, hook = 0;
        int on = 0, absDev = 0;
        RelayGetStats(&raw, &ign, &rel, &btn, &hook, &on, &absDev);
        printf("t=%4dms  raw=%ld  ign=%ld  moved=%ld  btn=%ld  hook=%ld  on=%d  abs=%d\n",
               (i + 1) * 500, raw, ign, rel, btn, hook, on, absDev);
        fflush(stdout);
    }
    RelayStop();
    printf("stopped cleanly\n");
    LogClose();
    return 0;
}
