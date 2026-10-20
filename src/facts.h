// ============================================================================
//  facts.h  —  实测事实与常量（**只放实测确认过的东西**，每条都注明证据来源）
//
//  这个文件是新项目的"第一等公民"：重写代码可以，重写这些结论不行。
//  证据文件都在旧项目 OeWinFix\analysis\ 里，索引见 docs/evidence.md。
// ============================================================================
#pragma once

// ---------------------------------------------------------------- 目标客户端
// 广播渲染窗口的窗口类。实测：真实客户端(ScreenRender.exe)与测试替身都用它，
// 而且是"哪个窗口铺满屏"最可靠的判据（比进程名靠谱：README 里 v1.1 就是按进程名漏判的）。
#define BCAST_WINDOW_CLASS      L"MultiRenderWindowClass"

// 教师端广播时会把输入桌面切到这个名字的专用桌面（不是 Winlogon 那种系统桌面）。
// 证据：live-raw.log 里 299 次采样 inputDesktop=NetTeachingDesktop；
//       MultiClient.exe 里 "dmt_screen_open" -> "Default"/"NetTeachingDesktop" 字符串成对出现，
//       StopMulticast 时再切回；ScreenRender.exe 导入 OpenDesktopW 按名字打开它建窗口。
#define TEACH_DESKTOP_NAME      L"NetTeachingDesktop"
#define TEACH_DESKTOP_NAME2     L"OeTeachingDesktop"   // 兜底：其它版本可能叫这个

#define APP_DIR_DEFAULT         L"C:\\Program Files (x86)\\Os-Easy\\os-easy multicast teaching system"

// ---------------------------------------------------------------- 鼠标白名单魔数
// 客户端在广播期间装 WH_MOUSE_LL(StudentLogic.dll RVA 0x80190)，只放行
// dwExtraInfo == 0x12452（教师端远程注入的标记），其余真实事件 return 1 吞掉。
// 反汇编：cmp dword[ecx+0x14],0x12452 ; je pass ; mov eax,1 ; ret
// （analysis/07-disasm-StudentLogic-mousehook.txt）
// 实测：广播中注入 extraInfo=0 -> 位移 0(FROZEN)；extraInfo=0x12452 -> 位移 97,60(MOVED)
#define MOUSE_MAGIC             0x12452u

// ---------------------------------------------------------------- 键盘驱动
// LockKeyboard.dll -> DefineDosDeviceW(L"Kbd", L"\Device\KeyboardClass0") -> 开 \\.\Kbd
//   DisableKeyboard: DeviceIoControl(0x000B2004)   封锁（我们不下发封锁，只记录备用）
//   EnableKeyboard : DeviceIoControl(0x000B2008)   释放
// 设备本体 \\.\OeKbdFilter（KbFilter.sys 的符号链接）。
// 实测：\\.\OeKbdFilter 可打开，0x000B2008 下发返回 OK，键盘立即恢复。
#define KBD_DEV_SELF            L"\\\\.\\OeKbdFilter"
#define KBD_DEV_FALLBACK        L"\\\\.\\Kbd"
#define KBD_DOS_NAME            L"Kbd"
#define KBD_DOS_TARGET          L"\\Device\\KeyboardClass0"
#define KBD_IOCTL_RELEASE       0x000B2008u

// ---------------------------------------------------------------- 窗口化
// 改窗口样式后必须用 SWP_NOSENDCHANGING(0x0400)：
// 实测不加时 SetWindowPos 返回成功但尺寸被 ScreenRender 在 WM_WINDOWPOSCHANGING 里顶回 1920x1080。
#define SWP_NOSENDCHANGING_BIT  0x0400

// 窗口化后的默认几何：工作区 80% 居中（与旧项目一致的观感）
#define WIN_SCALE_NUM           4
#define WIN_SCALE_DEN           5

// ---------------------------------------------------------------- 其它实测结论（写代码时的前提）
// * Student.exe 被 ProcFireWall 驱动保护：OpenProcess 返回 5(拒绝)，连它的模块表都读不到
//   -> 任何"改客户端内存"的方案对主客户端无效，不要依赖它。
// * 在"非输入桌面"上 SetWindowsHookEx(WH_MOUSE_LL) 返回 5
//   -> 我们的钩子只能装在当前输入桌面；这也解释了客户端必须先把桌面切过去再装钩子。
// * 客户端的钩子会周期性重装，重装后它排在我们前面：我们的钩子收不到事件
//   -> 位移/按键必须走 Raw Input（不受钩子顺序影响），钩子只做"抢先丢弃原件"的优化。
// * 只信物理设备：Raw Input 包头 hDevice == NULL 表示注入输入（含我们自己注入的）
//   -> 不过滤会形成"注入->被自己收回来->再注入"的反馈环，把指针顶死在屏幕角落。
// * 鼠标输入被吞掉时坐标**不累积**（注入 6 步只走 22px，白名单对照 165px）
//   -> 逐事件复刻绝对坐标会严重欠走，必须重放"设备相对增量"。
// * 桌面非活动时 PrintWindow(PW_RENDERFULLCONTENT) 仍能抓到窗口内容
//   -> 夺回自己桌面后，用镜像窗口继续显示广播画面是可行的（实测 98% 非黑、动图 99.96% 帧变化）。
