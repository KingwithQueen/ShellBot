# ShellBot 1.0.10 Windows

正式包 ShellBot-1.0.10-windows-x64.zip。经典 mstsc 最小化时可能停止远端渲染，而控制暂停也会阻止新观察租约。本版分别处理客户端渲染与服务器人工暂停。

- 新增可独立复制的 docs/tools/ShellBotRdpClient.ps1，默认 Status 只读；Apply 先备份原设置，再按用户/机器范围设置 RemoteDesktop_SuppressWhenMinimized=2，Restore 恢复原类型/值并保护后来的用户修改。客户端自己在方便时重连；工具不关闭连接、不重启，本轮未将设置应用到真实客户端。
- Windows desktop_lease 新增 access:observe，暂停时仍可截图和列窗口，不安装输入钩子，也不能聚焦、输入或写剪贴板。默认控制行为和人工暂停保护保留。
- host_capabilities 和桌面执行错误附当前会话、RDP/控制台协议、窗口站与输入桌面诊断，观察可用与输入可用分别报告。新增 --desktop-session-status 只读独立诊断命令。
- 工作进程始终绑定自身会话，不跳到控制台或其他用户，不控制锁屏、断开会话或安全桌面。Windows 优先；独立 Mac 修改保留，Mac/Linux 尚未重新移植验收。

操作见 [RDP 说明](REMOTE_DESKTOP.md)。需在发起 mstsc 连接的电脑运行客户端工具；升级 ShellBot 后重连 MCP 刷新 access 参数定义。普通桌面和模拟 RDP 检查、隔离注册表以及后台回归不能代替真实客户端改设置并重连/最小化的验收；准确范围见 validation-summary.json。
