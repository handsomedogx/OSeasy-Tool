// ============================================================================
//  uitest.cpp — 只用来"看"状态面板长什么样（不启动守护、不碰正在跑的 OeWinFix2.exe）
//
//  把 statusui.cpp 需要的其它模块用桩顶掉，中继用真代码但保持关闭（鼠标完全交给系统）。
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

#include "../src/config.h"
#include "../src/log.h"
#include "../src/relay.h"
#include "../src/statusui.h"

// ---- 项目里由 util/desktop/windowize/kbd/mirror/bcast 提供的东西，这里给桩 ----
OPTIONS g_opt = { 1,1,1,0,0,1,1,1, 200,250,400,10, 0,0,0,0, 1, 0,0,0,0,0 };

const wchar_t* OwnDesktopName(void)   { return L"Default"; }
const wchar_t* InputDesktopName(void) { return L"Default"; }
BOOL  DesktopIsOurs(void)             { return TRUE; }
BOOL  SwitchToClassDesktop(void)      { return FALSE; }
BOOL  ReclaimOwnDesktop(void)         { return FALSE; }
void  DesktopHoldReclaim(DWORD)        { }
int   ReclaimCount(void)              { return 0; }

BOOL  HasBroadcastWindow(void)        { return TRUE; }
BOOL  BroadcastWindowOnOwnDesktop(void) { return TRUE; }   // 模拟 B 方案生效：备用入口应隐藏
int   WindowizeToggleMaximize(void)   { return 1; }        // 路线 C：面板"看大画面"按钮
int   WindowizeNowManual(void)        { return 0; }
int   WindowizeFixCount(void)         { return 12; }

BOOL  KbdAvailable(void)              { return TRUE; }

void  MirrorStart(void)               { }
void  MirrorStop(void)                { }
HWND  MirrorWindow(void)              { return NULL; }
void  MirrorSetUserHidden(int)        { }

int   BcastFixCount(void)             { return 3; }

// statusui 的"退出程序"按钮：壳里就是关掉自己
void  AppRequestExit(void)            { PostQuitMessage(0); }

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int)
{
    LogInit(L"bin\\uitest.log");
    RelayLoadSettings();
    RelayStart();          // 只跑透明监视（g_opt.mouseRelay=0 -> 不装钩子、不接管）
    StatusUiShow();

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    RelayStop();
    return 0;
}
