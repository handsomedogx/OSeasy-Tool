# 实测证据（OSeasy-Tool 的依据）

> 全部数据来自 2026-10-12 / 10-13 在同一台学生机（Windows 10 x64，Os-Easy 多媒体网络教室
> V10.9.1.5145）上的测量。原始日志在旧项目 `OeWinFix\analysis\` 与 `OeWinFix\bin\OeWinFix.log`。
> 本文件只记结论与数字；复现方法见每节的"怎么复现"。

---

## 1. 广播 = 独立桌面 `NetTeachingDesktop`（代码级）

* `MultiClient.exe` 里 `App::StartMulticast` / `dmt_screen_open` 紧邻的桌面名字符串成对出现：
  `Default` … `NetTeachingDesktop`；`App::StopMulticast` / `dmt_screen_close` 时再切回。
  另一处还有 `Default` / `Winlogon` / `NetTeachingDesktop` 三元组，以及 `other desktop` 分支。
* 导入表：`MultiClient.exe` 有 `CreateDesktopA/OpenDesktopA/OpenDesktopW/SwitchDesktop/OpenInputDesktop/GetThreadDesktop`；
  `ScreenRender.exe` 有 `OpenDesktopW`（按名字打开教学桌面，把渲染窗口建在那上面）；
  `StudentLogic.dll` 有 `OpenInputDesktop/SetThreadDesktop/SwitchDesktop/CloseDesktop`；
  `systemoper.dll` 有 `CreateDesktopA/SwitchDesktop/SetThreadDesktop/OpenInputDesktop`。
* 运行时：旧项目监控脚本连续采样 **299 次** `inputDesktop=NetTeachingDesktop`（自己桌面 `Default`），
  且 `[NetTeachingDesktop]` 下能枚举到 `ScreenRender.exe` 的 `MultiRenderWindowClass` 窗口。

**怎么复现**：`OeWinFix\bin\MouseProbe.exe list NetTeachingDesktop`（广播中执行）。

**含义**：自己的桌面在广播期间收不到任何输入 —— 这是"鼠标不能移动"的最底层原因。
`SwitchDesktop(自己桌面)` 实测返回成功并立即生效（旧项目日志"夺回自己的桌面 (第 N 次)"）。

---

## 2. 鼠标锁 = `WH_MOUSE_LL` 白名单（0x12452）

钩子过程：`StudentLogic.dll` RVA `0x80190`（`OeWinFix\analysis\07-disasm-StudentLogic-mousehook.txt`）：

```asm
cmp dword [ebp+8], 0            ; nCode == 0 ?
jne pass_through
mov  eax,[ebp+0x10]
mov  ecx,[ebp-4]
cmp  dword [ecx+0x14],0x12452   ; dwExtraInfo == 教师端注入标记 ?
je   pass_through
mov  eax,1                      ; 其余真实事件: return 1 = 吞掉
jmp  epilogue
pass_through: call CallNextHookEx
```

注入位移测试（`MouseProbe.exe cursor 6`，每次 +12,+8 ×6）：

| 时刻 | `extraInfo=0` | `extraInfo=0x12452` |
|---|---|---|
| 广播中（14:0x） | **位移 0（被吞）** | **位移 97,60（放行）** |
| 广播结束后同一测试 | 位移 95,63（放行） | 位移 98,65 |

**关键**：钩子吞事件时**坐标不累积** —— 这是同一时期测到的：
逐事件复刻绝对坐标的重放，6 步只走 **22px**，而白名单对照是 **165px**。
所以中继必须读"**设备相对增量**"，不能复刻坐标。

> **2026-10-20 更正（本条结论的一半已被推翻，实现已改）**：
> "相对增量"仍然对，但**不能再把增量用 `SendInput` 逐包重放**——
> 补出来的事件必须穿过客户端那条串行低级钩子才生效，125Hz+ 鼠标时发送速度超过链条消化速度，
> 事件在输入队列里积压：总位移正确但被匀速排空，观感是"实际移了一大段、指针在后面慢慢追上来"。
> 实测同一段操作：旧法 5 秒往链上塞 975 个事件；改成 `SetCursorPos` 直驱光标后只有 4 个（全是别人发的），
> 延迟消失。现在的实现见 `src/relay.cpp` 顶部注释。

---

## 3. Raw Input 在锁期间仍然活着

`MouseProbe.exe rawcount 20`（物理鼠标来回移动 20 秒，广播中）：

```
raw 包=147    LL 钩子可见事件=101    光标实际位移=2195
```

* Raw Input 不受 `WH_MOUSE_LL` 影响 → 中继的数据源。
* 这也说明"物理事件部分能进系统"，但指针被钩子/定位逻辑压着走 → 手感"卡、跳"。

---

## 4. 客户端的钩子会周期性重装，且排在我们前面

* 旧项目日志：我们的钩子装了以后 `hookSeen` 长时间为 0，而 raw 包照常增加（设备在动）。
* 结论：**全局 LL 钩子的调用顺序是"最后装入先被调用"，但客户端会重装**，重装后它在我们前面，
  我们的钩子根本不会被调用（实测重装 3 次仍如此）。
* 因此 OSeasy-Tool 的设计是：**位移/按键/滚轮全部走 Raw Input**，钩子只做"抢先丢弃原件"的优化，
  不是必需品。日志里会明确写一句"客户端钩子始终在最前面 —— 无妨"。

---

## 5. 反馈环：注入输入会以 `hDevice == NULL` 的 raw 包回来

回归脚本一次运行中，注入的测试事件产生了 **89 个** `hDevice == NULL` 的 raw 包（被中继忽略）。

* 不过滤 → 注入被自己收回 → 再注入：指针被顶到屏幕角落并钉死
  （现象：中继一开，连白名单 `0x12452` 都变成"位移 0"；关掉中继立刻恢复 94,25）。
* 过滤后：回归断言 `C1`（中继接管中白名单仍放行）与 `D2`（注入包被忽略）同时成立。

---

## 6. 窗口化：必须带 `SWP_NOSENDCHANGING`

* 不加该标志时，`SetWindowPos` 返回成功，但尺寸被 `ScreenRender` 在 `WM_WINDOWPOSCHANGING`
  里顶回 1920×1080。
* 客户端在广播期间会反复重设位置/置顶（旧项目日志里同一窗口每秒数次 `window windowed`）→ 必须周期纠正。
* 最小化窗口的 rect 是 `(-32000,-32000,-31840,-31972)`：把它当"漂移到屏幕外"去摆位会导致
  每秒 5 次空转、永不收敛（旧项目踩过）。OSeasy-Tool 的做法：最小化只修样式不碰位置尺寸，
  并对"摆了但没变化"的窗口静默 5 秒。

---

## 7. 镜像（已删除）：桌面非活动时 `PrintWindow` 仍能取到画面

* 带动图的假广播窗口（`FakeBox --live --desk NetTeachingDesktop`）：两次抓图
  **99.96% 采样发生变化**（画面在刷新）。
* 真实广播窗口在其桌面非活动时：**98.4% 非黑**（抓到的是老师桌面的内容），另有 0.61% 采样在 3 秒内变化。
  截图：[mirror-live.png](mirror-live.png)。

**怎么复现**：`OeWinFix\bin\MouseProbe.exe liveness <桌面名> 3`。

> **这一节只留作实测记录，功能本身已于 2026-10-20 删除**（`src/mirror.*`、`--no-mirror`、
> `--mirror-fps`、面板与托盘里的"镜像窗口"入口一起拿掉）。原因不是它不工作，而是 B 方案把广播
> 窗口搬回**你自己**的桌面以后它没有使用场景 —— 同一幅画面出现两次。要回滚看 git 历史。

---

## 8. 已知无效/危险的路（不要再走）

| 想法 | 为什么不行（实测） |
|---|---|
| 改客户端内存让钩子失效（`74→EB`） | 主客户端 `Student.exe` 被 `ProcFireWall` 驱动保护：`OpenProcess` 返回 **5**，模块表读不到。补丁在无保护进程（替身）上有效，对主客户端无效 |
| 卸载客户端钩子（`UnhookWindowsHookEx`） | 只能在持有钩子句柄的进程里调用 → 需要注入，同样被驱动挡住 |
| 在非输入桌面上装我们的全局钩子 | `SetWindowsHookEx(WH_MOUSE_LL)` 返回 **5**（这也是客户端必须先切桌面再装钩子的原因） |
| 钩子内部同步调用 `SendInput` | 死锁原始输入线程，进程卡成僵尸（实测踩过，两次进程都杀不掉） |
| 逐事件复刻绝对坐标 | 坐标不累积，严重欠走（见 §2） |
| 依赖"我们的钩子被调用" | 客户端重装后它排我们前面（见 §4） |

---

## 9. 【2026-10-13 血泪】全局低级钩子必须装在"会泵消息"的线程上

**现象**：`OSeasy-Tool` 运行期间**整机鼠标发卡**，卡到"点不动托盘箭头、双击打不开文件"。
而且当时中继是**关着**的（`重放=0`）——因为我把"只读监视"也做成了要装钩子。

**机制**：`WH_MOUSE_LL` 的回调是**由系统投递给"装钩子的那个线程"执行的**。那个线程必须
一直跑消息循环（消息检索期间系统才把回调塞进来）。我把它装在一个只会 `Sleep(1000)` 的
监视线程上 → **每一个鼠标事件都要等这个线程"醒过来"才被放行** → 全局卡顿。

**证据**：日志里 `raw`（原始输入包）5 秒 20~25 个，而 `钩子可见`（我们的钩子被调用次数）
只有 15→15→25→28，成块、不均匀；杀掉进程后鼠标立刻恢复。

**结论/规矩**：
1. **只读监视阶段一律不装钩子**（探测用 SendInput 就行，不需要钩子）；
2. 必须在装钩子的线程上用 `MsgWaitForMultipleObjects + PeekMessage/DispatchMessage` 等待
   （代码里是 `PumpAndSleep()`），而不是 `Sleep()`；
3. 钩子只是为了"丢弃原件"这一项优化，功能不依赖它（位移/按键全走 Raw Input）。

**顺带的教训**：探测"客户端是否在吞事件"必须做**差分**（普通注入 vs 白名单注入），
并且要等注入真的生效（实测 15ms 不够，要轮询到 ~120ms）。
只用"普通注入后光标没动"就下结论 → 会把"注入尚未生效"误判成"被吞"（我第一版就误报了）。