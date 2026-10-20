// ============================================================================
//  kbd.cpp — 键盘封锁解除（KbFilter.sys）
//
//  LockKeyboard.dll!DisableKeyboard -> DefineDosDeviceW(L"Kbd", \Device\KeyboardClass0)
//      -> 打开 \\.\Kbd -> DeviceIoControl(0x000B2004) 封锁
//  LockKeyboard.dll!EnableKeyboard  -> 同一个句柄 -> DeviceIoControl(0x000B2008) 释放
//  设备本体是 \\.\OeKbdFilter（KbFilter.sys 的符号链接），实测可直接打开并下发。
//
//  做法：每 400ms 下发一次释放。理由：封锁可能在"我们启动之前"就已下发，
//  而且客户端可能在广播过程中重新封锁；周期下发等于持续把它压回去（实测有效）。
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#include "kbd.h"
#include "config.h"
#include "facts.h"
#include "log.h"
#include "util.h"

static HANDLE OpenKbdDevice(void)
{
    HANDLE h = CreateFileW(KBD_DEV_SELF, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) return h;
    // 复刻原厂做法：先建立 DOS 设备名，再打开 \\.\Kbd
    DefineDosDeviceW(DDD_RAW_TARGET_PATH, KBD_DOS_NAME, KBD_DOS_TARGET);
    return CreateFileW(KBD_DEV_FALLBACK, GENERIC_READ | GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
}

BOOL KbdAvailable(void)
{
    HANDLE h = OpenKbdDevice();
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    CloseHandle(h);
    return TRUE;
}

BOOL KbdReleaseOnce(void)
{
    HANDLE h = OpenKbdDevice();
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    DWORD ret = 0;
    BOOL ok = DeviceIoControl(h, KBD_IOCTL_RELEASE, NULL, 0, NULL, 0, &ret, NULL);
    CloseHandle(h);
    return ok;
}

DWORD WINAPI KeyboardProc(LPVOID)
{
    if (!KbdAvailable()) {
        Log("keyboard: 设备 %ls / %ls 都打不开 —— 本机可能没装 KbFilter.sys（那封锁本来也不生效）",
            KBD_DEV_SELF, KBD_DEV_FALLBACK);
        return 0;
    }
    Log("keyboard: release loop on (IOCTL 0x%08X, 周期 %dms)", KBD_IOCTL_RELEASE, g_opt.kbdIntervalMs);
    int ok = 0, fail = 0;
    do {
        if (KbdReleaseOnce()) {
            if (ok == 0) Log("keyboard: release IOCTL 0x%08X OK（键盘驱动级封锁已解除）", KBD_IOCTL_RELEASE);
            ok++;
        } else {
            if (fail == 0) Log("keyboard: release IOCTL failed err=%lu", GetLastError());
            fail++;
        }
        if (g_opt.once) break;
        Sleep(g_opt.kbdIntervalMs);
    } while (!Stopping());
    Log("keyboard: loop exit (ok=%d fail=%d)", ok, fail);
    return 0;
}
