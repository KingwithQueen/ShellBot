# ShellBot 1.0.9 Windows

正式版包名为 ShellBot-1.0.9-windows-x64.zip，包含本地 ZIP 更新器。本轮仅维护 Windows；原有独立 Mac 改动保留，Mac/Linux 尚未重新移植验收。

- Windows 工具新增可选 response_mode:compact，结构化数据及图像保留，外层文本只保留一句摘要。批量归档每项仅留 ID、状态和必要错误，预算/主机信息整批一次；pending_task_ids 仅列调度预算内未尝试项。默认完整回执兼容。
- task_output 新增 payload_mode:text/bytes/both；规范文本和原始字节按需返回，显式模式不重复 text_utf8，原始字节留在任务中，可按独立文本游标安全分页。
- window_focus 回执增加调用进程、目标、前后台窗口的 session/PID/TID/桌面名和实际 API 返回值；不把未定义的 GetLastError 作为聚焦原因。
- 任务状态分别返回入口进程状态/退出码和整个 Job 的活动进程状态，并说明输出是否排空。入口退出而辅助进程继续时，任务仍按 Job 规则运行；退出码 259 不误判。

用法见 [命令任务](COMMAND_TASKS.md)、[桌面控制](DESKTOP_CONTROL.md)。升级后重连 MCP 客户端刷新工具定义。

验证范围：隔离真实 MCP/HTTP、中文日志和跨页原始字节、入口与 Job 辅助进程、批量归档和调度预算、审批/幂等模式切换、隐藏窗口的聚焦拒绝故障注入，以及 Release/Debug 后台回归、正式包解压/升级。聚焦 API 与输入在原生测试中被拦截，不改变实际焦点或注入按键；原反馈环境、真实应用聚焦/UAC、外部独立客户端与 Mac/Linux 仍需对应验收。准确结果随正式包 validation-summary.json 保存。
