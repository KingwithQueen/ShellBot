# Linux / macOS 远程桌面

第五阶段开发版。沿用共享 MCP 桌面工具、逐次项目策略和审批、按调用者绑定的短时租约；Windows 桌面后端和构建入口不变。Linux 另需 X11/XTest/XInput2/XFixes/XRandR、GIO、GStreamer app/video 开发包及 PipeWire GStreamer 插件；Mac 最低 macOS 13，使用 ScreenCaptureKit、辅助功能和系统剪贴板。所有平台需要 zlib。`SHELLBOT_DESKTOP=OFF` 只关闭管理窗口和托盘；Mac 桌面控制仍链接 Cocoa 等系统框架。

## 本机开启

1. 在管理界面选择项目，先停止后台，再进入“远程桌面 / 设置桌面权限”。屏幕查看、键鼠控制、剪贴板读取和写入分别开启；保存会保留原文件/命令策略、产生备份并增加策略版本。新项目默认关闭。普通目录表单编辑保留已有桌面权限。
2. 重新启动后台和项目，点击“连接桌面”。连接是异步的，界面显示等待系统确认、已连接或失败原因。没有自动连接、保存会话恢复令牌或后台批准系统弹窗。
3. Mac 在系统设置中允许运行此程序的屏幕录制权限；需要控制时，还需辅助功能和输入监控。允许后停止、重新启动项目并再次连接。系统可能按应用来源归属权限；开发二进制换路径/换签名后可能需要重新允许。
4. Wayland 通过 XDG RemoteDesktop/ScreenCast/Clipboard portal 选择一个共享显示器并确认输入与剪贴板权限。只读且不使用剪贴板时仅请求 ScreenCast；只读加剪贴板以零设备请求 RemoteDesktop，后端不支持这种组合时会拒绝连接，不扩大输入请求。拒绝、超时、关闭共享或流失败均保持不可用。不会转用 XWayland 绕过确认。
5. X11 使用当前会话已经建立的 DISPLAY/Xauthority。只在明确的 X11 会话连接；无显示服务器时拒绝。普通 SSH 环境不代表已连接桌面。

“暂停桌面”中断同一账户、同一显示会话所有 ShellBot 项目的租约。“本机恢复”不复活旧租约或观察记录；客户端必须重新申请租约并观察。项目暂停也暂停桌面控制，恢复项目请求不自动恢复桌面。关闭项目释放显示连接和租约。断开系统共享后请重启项目再连接。暂停入口直接使用项目的同账户私有 owner socket，避免等待远程输入请求结束。

无界面时可向运行项目的 `--owner-request FILE --data-dir PROJECT_DATA` 发送下列同账户本机请求。管理器模式也支持 `--project-owner ID --request FILE --manager-data-dir DIR` 转交。HTTP MCP 不提供这些所有者命令：

```json
{"command":"desktop-connect"}
```

其他命令为 `desktop-status`、`desktop-pause`、`desktop-resume`。不接受额外字段。配置例子见 `config/posix-desktop.example.json`，需要替换绝对路径；不要用于自动给旧项目增加权限。

## 客户端调用

仅有 SSH 时可运行 `ShellBot --desktop-permissions` 做只读检查；Mac 输出 screen_recording、accessibility、input_monitoring，不弹出授权窗口。Linux 检查当前进程的桌面环境变量；没有继承图形会话变量时不代表图形桌面本身不存在。该命令不会授权、启动项目或连接屏幕。Mac 的系统权限和 Wayland 的共享确认仍须在图形桌面进行，SSH/sudo 无法代替用户同意。

系统连接成功后，`tools/list` 才公开当前项目允许的桌面工具。`host_capabilities.desktop_status` 提供连接、暂停、独占租约、后端、平台限制和失败原因。审批方式为 always/workspace_auto 时先在本机查看审批详情并允许，再用原参数及 approval_id 重试；审批不会自动发送输入。

先调用 `desktop_lease` 的 acquire（唯一 request_key，duration_seconds 不超过项目上限，默认 30、最高 120 秒），保存 lease_id。随后 desktop_state / desktop_capture / window_list 返回 screen_id、显示区域、坐标单位和剪贴板序号。输入必须引用 30 秒内的观察，且租约、暂停世代、显示布局及受支持的前台窗口保护仍有效；每次动作后取得新观察。lease 的 renew/release/status 不等于项目、任务或 OAuth 的生命周期。

屏幕图仅在内存中编码并返回 PNG，长边最多 1600、PNG 最多 1 MiB。过大的 X11 源区域（16 百万像素以上）需裁剪。不会落盘截图或在审计中保存图像、键入文字及剪贴板内容。工具返回输入事件提交情况；不承诺应用已完成操作。相同 request_key 不重放输入，进程重启不恢复租约/审批/旧动作。

`desktop_click`、`desktop_drag`、`desktop_scroll`、`desktop_type`、`desktop_key` 使用该次观察报告的坐标。Mac 是屏幕点，Retina 比例单独报告；Wayland 是系统流的逻辑坐标，X11 是会话像素。根据 capture 的 coordinate_units_per_image_x/y 和裁剪区域换算，不能直接把压缩图片像素当桌面坐标。每次操作最多四个组合键；拒绝保留的紧急组合键和 Ctrl+Alt+Delete。拖动/键入途中遇到暂停或租约到期会停止，并尽力释放本次按下的键和按钮。

剪贴板只处理 UTF-8 文本，读取最多 32768 字节，写入最多 4096 字节；必须提供最新 expected_sequence。X11 支持 UTF8_STRING selection 和有限大小的直接传输，拒绝 INCR 大数据传输。Wayland 根据 portal 的所有者通知维护本进程序号，并按获得的 MIME 读取；通知型序号不能提供服务器原子的比较并交换。Mac 使用 NSPasteboard changeCount。跨应用写入仍可能存在操作系统级竞态；发现序号不匹配必须重新观察，不能盲目重试覆盖。

## 平台差异与验收边界

| 功能 | X11 | Wayland portal | macOS |
| --- | --- | --- | --- |
| 屏幕/鼠标/组合键 | 原生 X11/XTest | 系统授权的单显示器流和输入 | ScreenCaptureKit / CGEvent |
| 文本 | 当前键盘布局可直接表示的字符；不重映射全局键盘、不暗中改剪贴板 | Unicode keysym | UTF-16 Unicode 事件，含代理对 |
| 窗口列表、聚焦 | 当前账户窗口；身份和几何重新核对 | 不支持；明确报错 | 当前账户窗口；辅助功能唯一匹配后聚焦 |
| 人工输入自动暂停 | XInput2 已识别的物理输入设备 | 系统未提供全局观察能力；不支持 | 被动事件监听；系统权限撤销则断开 |
| 外部程序注入输入识别 | 不承诺 | 不支持 | 不承诺 |
| 紧急停止 | 人工输入/本机按钮 | 本机按钮或系统停止共享 | 人工输入/本机按钮 |

Wayland 还不能核对系统未公开的前台窗口、窗口进程归属、物理按键占用状态；`foreground_change_guard`、`input_target_identity_guard` 和 `human_input_pause_supported` 明确返回 false，只应在可信的当前账户桌面会话中开启控制。X11 和 Mac 会核对输入目标的当前账户身份；不会申请管理员执行，也不连接登录界面。X11 物理设备热插拔后需重启项目再连接，以更新设备识别。Linux 锁屏检查依赖桌面会话的 ScreenSaver D-Bus 接口，精简桌面缺少该接口时不能承诺完整锁屏检测。系统锁屏、权限撤销、共享关闭、后端错误和原生 API 拒绝必须单独验收；各桌面环境和系统版本能力不同。

测试源与验证报告按仓库约定留在本机、不进入 Git。`SHELLBOT_TESTS=ON` 的 remote-desktop 套件在两平台做未连接时拒绝访问的检查，在 Linux 新建私有 Xvfb，仅对测试窗口验证截图 PNG、鼠标、拖动、滚轮、组合键、文字、中文剪贴板、失效观察、租约和跨项目暂停。该测试不操作用户现有桌面。Mac 真实权限开启后的操作、Wayland 系统同意后的流/输入/剪贴板、多显示器/Retina、锁屏和物理人工中断，以及外部 MCP 客户端均须另记实机或人工验收；编译或 Xvfb 通过不能代替这些结果。
