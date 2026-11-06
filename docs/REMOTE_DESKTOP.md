# Windows RDP 桌面操作

适用于 Windows 1.0.10 及以后版本和经典“远程桌面连接” mstsc。连接在线、连接最小化、连接断开和用户锁屏是不同状态。需分开判断会话是否交互、客户端是否继续渲染，以及 ShellBot 输入是否暂停。网络在线不能证明这些条件都满足。

Windows 1.1.5 已提供无需聚焦的 WGC/PrintWindow 窗口截图；本机真实 mstsc 最小化下的看图和后台 UIA 操作已通过，见文末。本文件较早版本的屏幕 DC 限制不等于窗口捕获也不可用；客户端渲染设置仍可用于需要完整桌面/物理输入的情况。

Windows 1.1.7 进一步提供两条显式、无需虚拟机的 GPU 取图路径：已配置本机调试端口的 Chromium 页面截图，以及新启动的受支持 x64 D3D11 应用的提交前采集。Z620 断开并锁屏时已取得真实变化的画面，见本文件的 1.1.7 记录及 [GPU 取图说明](GRAPHICS_CAPTURE.md)。这不使登录界面、UAC 或物理键鼠可用。

## 发起连接的电脑：避免最小化时抑制渲染

Windows RDP 客户端最小化后可能停止图形渲染，即使网络仍在线，截图和界面自动化也可能失败。微软列出了该情况；SmartBear 的 TestComplete 文档给出当前用户/全部用户设置及明确的客户端位置说明。

- [Microsoft：最小化 RDP 与 UI 自动化](https://learn.microsoft.com/en-us/troubleshoot/power-platform/power-automate/desktop-flows/ui-automation/uipi-issues)
- [SmartBear：在最小化 RDP 窗口中运行自动化](https://support.smartbear.com/testcomplete/docs/testing-with/running/via-rdp/in-minimized-window.html)

**在发起 mstsc 连接的电脑执行，不是只在被控服务器执行。** 配套 [ShellBotRdpClient.ps1](tools/ShellBotRdpClient.ps1) 可单独复制到客户端，不依赖 ShellBot 安装。

在正式包根目录的 PowerShell 中只读查看：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\docs\tools\ShellBotRdpClient.ps1 -Action Status
```

应用到当前用户（默认无需管理员权限，先备份原始值和类型）：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\docs\tools\ShellBotRdpClient.ps1 -Action Apply
```

工具在 HKCU\Software\Microsoft\Terminal Server Client 的适用 32/64 位注册表视图写入 DWORD `RemoteDesktop_SuppressWhenMinimized=2`。如需全部用户可显式指定 `-Scope AllUsers`，需要相应管理员权限；工具不自动提升权限。默认备份保存在客户端 LocalAppData\ShellBot\RdpClient 下，可用 `-BackupPath` 指定一个尚不存在的文件，旧备份不会被覆盖。

应用成功后，**在方便时完全退出现有 mstsc 客户端，再重新启动并连接**，再测试最小化。工具不强制关闭当前连接、不重启电脑、不改变长期脚本执行策略，也不解锁服务器或自动恢复 ShellBot 输入。Windows App、其他 RDP 客户端和组策略不保证采用该经典 mstsc 设置。若有兼容性问题可按输出的备份路径恢复，未改动的其他注册表值会保留：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\docs\tools\ShellBotRdpClient.ps1 -Action Restore -BackupPath "上一步返回的备份完整路径"
```

Restore 只接受原电脑/账户的该固定设置备份；若当前值既不是本工具设置的 2、也不等于原值，会拒绝覆盖后来的用户修改。原值不存在时只删除本设置，不删除整个注册表键或其他设置。

## 被控电脑：分开观察与控制

ShellBot 应在目标 RDP 用户的同一交互会话中运行。连接在线并不意味着它与服务或旧控制台进程处于同一会话。每个 RDP 会话有自己的 WinSta0；本版始终绑定工作进程的 session，不将 console_session_id 当作控制目标。

`host_capabilities.desktop_status` 新增 session、observation_available、input_available、input_block_reason、observation_leases_supported。session 包含连接状态、协议、进程会话、控制台会话、窗口站、线程桌面和输入桌面；无法查询的值为 null/unknown。interactive_desktop_available 表示会话/桌面满足条件，不保证 mstsc 正在渲染，实际抓屏结果仍以 capture_diagnostic 为准。

暂停输入时仍可获取观察租约：

```json
{"action":"acquire","access":"observe","duration_seconds":60,"request_key":"observe-new-1"}
```

将返回的 lease_id 用于 desktop_state、window_list、desktop_capture，以及项目显式允许的 clipboard_read。观察租约不安装键鼠钩子/热键、不解除人工暂停，不能聚焦、输入、移动鼠标或写剪贴板；renew 保留权限，不能升级，改变 access 前要释放租约。观察租约仍受项目权限、调用方绑定、会话和期限保护，并与控制租约互斥。

聚焦/按键需要原来的控制租约（省略 access 或 access:control）。先释放旧租约，在 ShellBot 托盘由所有者恢复控制，完成 mstsc 最小化后再发出新的控制请求，避免人和 AI 同时操作远端键鼠。实际焦点仍受 Windows 前台规则约束，失败查看 focus_diagnostic；不会强制抢焦点或越过 UAC。

在 RDP 内的终端可独立运行：

```powershell
.\WinShellBot.exe --desktop-session-status
```

该命令不需要管理器、项目或租约，只读报告当前进程会话。将它与 MCP 中工作进程的 session 对照，能区分跨会话、锁屏/安全桌面、暂停和仍在线但渲染失败。

## 验证范围

本版通过隔离注册表 Apply/Restore、真实普通桌面 MCP 观察租约与 PNG、原生私有暂停事件及模拟 RDP/不同控制台会话检查。没有修改真实客户端注册表、没有关闭/切换用户 RDP、没有改动现有安装。设置生效后“重新连接并最小化”的端到端行为仍需在连接发起电脑上验收。

断开、注销、锁屏和安全桌面不会因 DWORD 2 而成为可输入桌面。若要跨用户/跨会话或完全无人值守，需要另外设计明确绑定会话的代理及其生命周期；本次不引入 SYSTEM 桌面控制服务。

## 1.1.2 GUI 启动与恢复

RDP 客户端最小化不会一律阻止 GUI 进程创建。是否启动、目标窗口是否最小化、是否取得真实前台，以及能否取得像素是四种结果，应分别核实。应用安装在本地不决定其会话或前台资格。

在普通 current_user 项目中给 task_start 增加 `launch_mode:"gui"`，其余 workspace_id、executable（绝对本地 EXE）、argv、request_key、host_instance_id 与原任务一致。1.1.7 允许在自身同用户会话已锁定或断开时，向 WinSta0/Default 创建普通 GUI；创建不以截图或 OpenInputDesktop 成功为前提。在当前进程具备资格时可以向子进程交接前台权限，但不保证锁屏、断开或最小化时取得真实前台；失败不会提升身份或跨会话重试。后台模式仍默认，受限/管理员 GUI 模式不支持。启动不登录、不解锁，不操作锁屏或 UAC 桌面；后续观察、后台 UIA、聚焦与输入分别检查对应条件。

**GUI 模式仍是受管理任务。默认超时仍为 300000 ms（5 分钟），可在项目上限内显式指定 timeout_ms；任务取消、超时和工作进程停止仍会回收所属 Job。** 它不是脱离管理、永久运行的应用启动器。单实例程序可能把启动请求交给已有进程，新入口退出不代表窗口不存在。按 window_list 的实际窗口/进程身份选择目标，不只凭新入口 PID 推断结果。

1.1.7 的 `status:"starting"` 是非终态，先查询原任务的 startup 阶段及 process_resumed；`process_created:true` 也不证明窗口存在或已聚焦。`foreground_permission_granted` 只说明此次授权交接的 API 结果，不是永久前台权限。输入桌面可交互时，接着取得控制租约、获取新 window_list，再调用：

```json
{"lease_id":"当前控制租约","screen_id":"最近观察编号","window_id":"该观察中的目标编号","request_key":"新的操作键","restore":true,"wait_ms":1500}
```

这是 window_focus 的 arguments。window_list 新增 `minimized` 字段；默认先异步恢复最小化应用，再请求激活。可设 restore:false 避免恢复，或使用 0～3000 ms 的 wait_ms 控制整个恢复/确认等待。等待期间继续检查身份、布局、租约和人工暂停；不绑定输入队列、不伪造 Alt，也不永久置顶。后台进程仍可能被 Windows 拒绝激活。恢复或聚焦尝试后重新观察，不能继续使用旧屏幕编号和坐标。

成功回执确认 `foreground_observed:true`，并明确 `screenshot_verified:false`；截图另行调用 desktop_capture。窗口能显示不能证明最小化 mstsc 时持续渲染，截图失败也不能反推没有成功启动 GUI。

真实验收用相同 EXE、账户、权限和会话做两组对照：交互终端延迟启动，以及 ShellBot GUI 模式延迟启动；分别记录连接可见、mstsc 最小化期间和恢复后的 PID/窗口/前台/会话。必须记录最小化期间的 GetForegroundWindow，而不是只看重新打开 mstsc 后的画面。开发用本地 GUI 探针只在显式 --window 时显示窗口，15 秒后出现、45 秒后自动关闭并按 250 ms 记录前台，自动化模式不创建可见窗口。原生恢复/聚焦测试拦截 API，不能代替这一真实验收。

## 1.1.3 元素操作

UIA 提供独立于像素坐标的元素树和 pattern 动作，使用说明见 [交互工具](INTERACTIVE_TOOLS.md#windows-ui-automation)。1.1.3 动作要求目标窗口已经前台；1.1.4 增加下面的显式后台模式。它仍要求可用的同用户交互桌面，保留人工暂停和目标身份检查，不保证所有应用提供完整可访问性信息。

## 1.1.4 实机对照与后台语义操作

2026-11-05 在用户正在使用的 mstsc 会话进行隔离测试：普通未提升账户，ShellBot 在 RDP session 1 的 WinSta0/Default，控制台 session 2 仅作为诊断，未用于控制。只对自建测试窗口操作，没有更改真实 RDP 客户端设置、切换会话或部署现有安装。

| 实际状态 | 窗口/控件 | 后台 set_value / invoke | window_focus | GDI 抓屏 |
| --- | --- | --- | --- | --- |
| RDP 展开且解锁 | 可读取 | 中文写入读回、按钮触发一次 | 成功，确认真实前台 | 成功 |
| RDP 最小化且解锁 | 可读取，窗口本身未最小化 | 中文写入读回、按钮触发一次 | 失败，GetForegroundWindow 仍为空 | StretchBlt / fallback.BitBlt 错误 5 |

最小化期间 WTS 保持 active，锁定标志为 unlocked，OpenInputDesktop 仍返回 Default，但 GetCursorPos 返回错误 5。这证明“窗口可创建/控件可用”与“物理前台/像素可用”需要分开判断。测试首次切换时人工输入触发暂停，后续动作被正确拒绝；用户明确允许仅本次恢复控制后完成后台测试，结束即恢复暂停，没有自动绕过人工接管。

此前还观察到 WTS active 但 session 已锁定、输入桌面被拒绝的状态。1.1.4 增加 WTSSessionInfoEx 的 lock_state/session_flags/lock_query_win32_error；当时已知锁定即拒绝桌面操作，即使 WTSActive 或输入桌面名称看似正常。这仍是默认 interactive 模式的规则；1.1.6 起普通应用可另行显式申请下文的 application_background 租约。查询未知仍保留输入桌面检查，不把未知当作解锁证明。独立诊断命令、能力报告及操作失败中的 session 同时返回光标查询结果和前台是否存在；raster_capture_available:null 表示没有在状态查询中试抓屏，实际结果查看 desktop_capture.capture_diagnostic。

要在该最小化状态操作支持 UIA 的应用，取得控制租约，通过 window_list、uia_tree 选定目标后显式调用 uia_action 的 focus_mode:background。无需先 window_focus 或截图；后台动作不会因为无关窗口的前台变化过期，但继续检查观察期限、人工输入、布局和目标身份。默认 foreground 行为不变，仍需真实前台。控件 pattern 派发与应用结果分开报告，完成后刷新验证。

本机在 150% 缩放下还复现了旧版 UIA 返回 660×390 而窗口列表返回 990×585 的单位差异，现已让 UIA 辅助线程采用 Per Monitor V2，并在真实测试中确认两者均为物理像素。

本节 1.1.4 实测未取得最小化时的像素；后续窗口捕获及 1.1.7 配套取图不再要求先取得前台。需要真实按键/坐标或聚焦时，仍应在**发起连接的电脑**评估本文件前述 mstsc 渲染设置，退出 mstsc 后重新连接验收；本次没有修改客户端设置或声称此配置已经验收。后台 UIA 不操作登录/UAC 界面，不提供缺失的可访问性 pattern。

依据：[WTSSessionInfoEx 锁定标志](https://learn.microsoft.com/en-us/windows/win32/api/wtsapi32/ns-wtsapi32-wtsinfoex_level1_w)、[微软关于最小化 RDP 的 UI 自动化限制](https://learn.microsoft.com/en-us/troubleshoot/power-platform/power-automate/desktop-flows/ui-automation/uipi-issues)、[UIA control patterns](https://learn.microsoft.com/en-us/windows/win32/winauto/uiauto-controlpatternsoverview)。上表为本机自建 Win32 测试窗口的真实结果，不扩展为其他应用或机器的完整验收。

## 1.1.5 最小化时的窗口截图和后台操作

2026-11-05 在同一机器、同一普通用户 RDP 会话继续实测。用户明确确认将 mstsc 最小化约 90 秒后恢复；测试前后均为 active/unlocked、WinSta0/Default，GetForegroundWindow 为空且 GetCursorPos 返回错误 5。没有改客户端注册表、关闭/切换会话、安装显示驱动或部署现有安装。

| 专用窗口 | 捕获后端 | 实际结果 |
| --- | --- | --- |
| 普通 Win32/GDI 控件 | WGC、PrintWindow、auto | 能看到完整窗口；后台中文写入读回、按钮单次调用，以及两次操作后的新画面均通过 |
| D3D11 GPU 子画布与标准控件 | WGC、auto | GPU 色块仅由实际 D3D11 Clear/Present 绘制；同样通过后台中文写入/按钮调用及画面更新核对 |

每类窗口先截图，再执行两次独立按钮操作，画布依次为橙、蓝、橙。逐后端解码 PNG，检查当前色块像素存在、旧色块像素消失，并核对应用自身的按钮计数和 UIA 中文值，排除了只返回 API 成功、旧帧或空白图的情况。测试窗口从未要求前台，两个窗口重叠时也直接捕获各自表面。最终构建的同程序副本再次完成了完整链路。

首轮测试因检测到人工输入而中断，随后用户明确允许仅这轮恢复一次控制，完成后重新暂停。最终构建复核没有调用恢复暂停接口。工具继续拒绝人工暂停期间的控制，不以重放动作掩盖失败。

现在可采用 `window_list → desktop_capture(window_id, screen_id, capture_backend:auto) → uia_tree → uia_action(focus_mode:background) → 刷新并重新截图`。具体字段和示例见 [交互工具](INTERACTIVE_TOOLS.md#不聚焦的窗口截图115)。相同 window_id 只对产生它的观察有效，每次新的操作用新的 request_key。

这条路径已在本机解决“mstsc 最小化时看不到窗口画面”的实际问题，不再要求先成功 window_focus 或先取得屏幕截图。窗口截图不等于整个桌面可抓取，也不改变实际键鼠的前台要求。UIA 控制能力取决于应用的控件提供者；登录/UAC 界面、被系统排除的捕获内容及不提供可操作控件的程序不由本模式绕过。锁屏后的普通应用另见下文显式后台模式。其他应用、驱动、多屏/HDR与独立 Windows 机器仍应分别验收。

实现依据：[CreateForWindow](https://learn.microsoft.com/en-us/windows/win32/api/windows.graphics.capture.interop/nf-windows-graphics-capture-interop-igraphicscaptureiteminterop-createforwindow)、[CreateFreeThreaded](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.direct3d11captureframepool.createfreethreaded)、[PrintWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-printwindow)。WGC 无需将被遮挡窗口置前的成熟用法也见 [Microsoft winapp UI](https://learn.microsoft.com/en-us/windows/apps/dev-tools/winapp-cli/ui-automation)。

## 1.1.6 断开并锁屏时的应用后台模式

以下保留 1.1.6 的历史验收，GPU 旧画面失败并未改写为通过。2026-11-06 通过 Z620 任务接口，在真实 disconnected/locked、进程第 3 会话、Default 桌面、输入桌面访问错误 5 的环境运行独立候选版。现有安装未被替换，电脑始终保持锁定，没有登录、解锁、切换会话或修改客户端设置。旧版常规后台命令可运行，但桌面不可用，64 条操作回执也仍占满；新版使用显式 application_background 租约。

| 实测能力 | 断开并锁屏结果 |
| --- | --- |
| 常规 interactive 租约 | 拒绝，保留原来规则 |
| 应用窗口枚举、UIA 树 | 可用，仅本用户会话的普通应用 |
| 后台中文写入/读回、按钮调用 | 两类自建窗口均通过，按钮计数 0→1→2 |
| 普通 GDI 窗口的 PrintWindow/auto | 三个阶段均取得当前画面，橙→蓝→橙像素核对通过 |
| WGC | 此锁屏/断开环境无可用帧，auto 回退 PrintWindow |
| D3D11 画布 | 语义操作可用；PrintWindow 曾返回旧色块，GPU 当前画面验收不通过 |

后台模式用法见 [交互工具](INTERACTIVE_TOOLS.md#锁屏时的应用后台模式116)。它访问普通应用的语义对象和绘图接口，不把锁屏/UAC 桌面变成可操作界面。默认交互模式和真实键鼠继续要求可用的输入桌面，状态变化会使后台租约失效。微软的 [winapp UI 文档](https://github.com/microsoft/winappCli/blob/main/docs/ui-automation.md)也区分锁屏友好的 pattern 操作与依赖输入桌面的注入/截图；[Windows 桌面对象说明](https://learn.microsoft.com/en-us/windows/win32/winstation/desktops)解释了 Default 与安全桌面的隔离。

## 1.1.7 锁屏时的 GPU 新画面

优先使用以下两条无需虚拟机的显式路径。它们采集应用内容，不依赖窗口取得前台，也不把锁屏改为可交互桌面。完整命令、权限、停止方式及限制见 [GPU 取图说明](GRAPHICS_CAPTURE.md)。

| 应用与前提 | 取图路径 | 已完成的 Z620 实测 |
| --- | --- | --- |
| 所有者已明确配置本机调试端口的独立 Chromium 自动化实例 | 只读 ShellBotChromiumCapture.ps1，指定端点与页面 target | 独立 Edge、Quadro K6000、D3D11/WebGL；三次截图各有 83520 个当前色块像素，旧色块为 0，未启用焦点仿真 |
| 新启动的同用户、普通权限 x64 D3D11 应用 | 显式 ShellBotGraphics 启动，采集提交前的受支持后缓冲 | 冻结的正式程序完成后台 UIA 中文写入/读回和两次按钮调用，橙→蓝→橙三张图均核实当前像素 |

D3D11 配套当前仅覆盖经典 D3D11CreateDeviceAndSwapChain 加 Present 的首个受支持交换链，非 MSAA、非 HDR 的 BGRA/RGBA8 后缓冲；不覆盖 Present1、Factory 创建交换链、D3D12、OpenGL、Vulkan 或跨进程 GPU 渲染。它只托管显式新启动的程序，不附着既有用户应用。Chromium 脚本只取所选页面内容，应用必须自己提供兼容调试协议；这不证明所有游戏或 Electron 都支持。

普通 desktop_capture 仍优先 WGC、兼容回退 PrintWindow，不会自动启用配套程序或注入应用。回执新增 `frame_freshness.status:"unverified"` 和 `application_update_verified:false`；PrintWindow 的 cached_surface_possible:true 明确提示旧合成面可能性。WGC 的 compositor_frame_age_ms 是合成器帧龄诊断，不等于应用已完成当前操作。应结合应用状态、动作后的帧序号/新鲜度及变化像素核实结果，不能只看 PNG 成功或时间戳变化。

最终核心另在 Z620 同一锁定/断开状态通过 100 个私有 PowerShell 脚本任务：输出逐项核对并归档，最终活动/实时记录均为 0、归档数为 100；初次回执 87 个 running、13 个 starting，观测到的最大启动耗时 297 ms。同一 request_key 重试的标记只写一行。真实 ConPTY 输出及结束、GUI 模式启动、后台 UIA 值写入/读回和普通 PrintWindow 的 frame_freshness:unverified 回执也通过，结束后会话仍为 locked/disconnected。该轮共 1254 项检查，早期探针的包装/命令解析失败保留，未作为启动超时改写。

上述实测解决了受支持路径的锁屏 GPU 看图问题；真实聚焦、键鼠和登录/UAC 界面仍不属于这些路径。整轮回归和原生测试另行记录，不由 GPU 或核心专项结果代替。

### 不覆盖的图形或物理输入需求

受支持 API 之外，或确实需要真实键鼠时，可以把自动化应用与 ShellBot 放进专用 Windows 虚拟机，保持虚拟机内有可渲染、已认证的交互会话；宿主机可以保持锁定。使用支持图形渲染的虚拟显示/GPU 配置，并在该环境单独验收截图和操作。普通 Windows 的多个任务视图桌面不能替代独立会话或虚拟机。

另一条成熟路线是用具备凭据管理的无人值守会话代理，按所有者预先配置的认证创建/维护 RDP 会话，再运行 ShellBot 和目标应用。这需要专用账号、会话和相应 Windows 许可，不能省略认证。[Power Automate 的官方说明](https://learn.microsoft.com/en-us/power-automate/desktop-flows/run-unattended-desktop-flows)明确其会创建受管理的 RDP 会话；Windows 10/11 上现存的活动或锁定用户会话会妨碍其无人值守运行，因此现有 Z620 会话不能直接当作已配置完成的无人值守环境。

本轮没有创建账号/虚拟机、保存登录凭据、注销用户、关闭锁屏或部署会话服务。虚拟机与无人值守代理是未覆盖需求的环境选择，不是本版 GPU 取图的唯一办法，也未作为本机已通过验收的方案。若 PrintWindow 内容不更新，应停止依赖旧图继续点击，改用可验证的 UIA 状态、上述明确适配的配套取图，或已验收的独立渲染会话。
