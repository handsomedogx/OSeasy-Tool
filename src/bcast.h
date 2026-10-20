// ============================================================================
//  bcast.h —— 广播参数改写（B 方案：不改客户端文件，运行时把"全屏广播"改成"窗口广播"）
//
//  依据（实测）:
//    * 客户端渲染器 ScreenRender.exe 的**命令行**里带着整条广播参数:
//        "…\ScreenRender.exe" {#decoderName#:#h264#,#fullscreen#:1,#local#:#10.69.46.23#,
//                              #port#:7778,#remote#:#229.1.46.60#,#teacher_ip#:0,#verityPort#:7788}
//    * 我们把同样参数(可改写)交给客户端自己的 ScreenRender.exe 重新拉起, 实测能正常
//      渲染老师画面（docs/bcast-self-launch.png）——即: 不需要替换客户端的任何文件。
//    * 全屏广播(#fullscreen#:1)才会带来"锁键鼠 + 切桌面"；窗口广播(#fullscreen#:0)
//      的会话里客户端不上锁（今天窗口广播期间实测: 普通事件直接放行）。
//
//  所以本模块只做一件事: 发现 #fullscreen#:1 -> 杀掉它 -> 用 #fullscreen#:0 重新拉起。
// ============================================================================
#ifndef OE_BCAST_H
#define OE_BCAST_H

void BcastStart(void);        // 启动监视线程
void BcastStop(void);
int  BcastFixCount(void);     // 累计改写次数（状态里显示）

#endif
