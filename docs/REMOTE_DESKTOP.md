# Windows RDP 桌面操作

适用于 Windows 1.0.10 和经典“远程桌面连接” mstsc。连接在线、连接最小化、连接断开和用户锁屏是不同状态。需分开判断会话是否交互、客户端是否继续渲染，以及 ShellBot 输入是否暂停。网络在线不能证明这些条件都满足。

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
