# ShellBot 1.1.1 Windows

正式包：`ShellBot-1.1.1-windows-x64.zip`。

本版针对 1.1.0 复测中仍出现的间歇性 `MCP -32603`，修正诊断的返回位置。有效工具调用的内部失败现在通过标准 `CallToolResult` 返回 `isError:true`、简短文字和 `structuredContent`，使用 HTTP 200 传递工具结果，避免只放在客户端可能丢弃的 JSON-RPC `error.data` 中。协议解析、认证和无效请求保留原有协议错误语义。

普通工具回执的 `structuredContent.request_trace` 包含服务端 request_id/trace_id、host_instance_id、失败边界和回执状态；compact 摘要也带关联编号。HTTP 响应增加 `X-ShellBot-Request-ID`，贯穿管理器和工作进程。项目忙的工具调用明确返回 `PROJECT_BUSY / not_started / request_queued:false`，不会排队或自动执行；工作进程通信中断仍返回 `unknown`，任务保留原幂等键，桌面输入先观察、禁止自动重放。

`host_capabilities` 增加按项目、调用方和当前工作进程隔离的内存诊断记录。默认显示最近 8 条，`request_diagnostics_limit:128` 可读取保留的完整窗口。窗口满后只淘汰最旧诊断，不占任务预算、不阻止新任务。记录关联编号、工具、时间、阶段、结果状态及 request_key/input_key 的 SHA256；不保留命令、stdin、文件正文或认证。重启后内存记录消失，内部失败仍写原有受保护审计日志。键哈希用于查询对应调用，不代替幂等键或执行证明。

`fs_stat` 将已授权路径的不存在、访问拒绝、共享冲突、类型不匹配及不支持的链接分别报告为 `NOT_FOUND`、`ACCESS_DENIED`、`SHARING_VIOLATION`、`TYPE_MISMATCH`、`UNSUPPORTED_FILE_TYPE_OR_LINK`，附实际 Win32/NTSTATUS。授权与路径边界检查仍先于源对象打开，不从无权限路径推断文件存在性。

本轮验证使用隔离项目、受控工作进程中断及拦截输入的原生桌面测试，不声称已复现或消除原客户端的全部间歇故障。只读检查线上仍为 1.1.0，且当前会话已断开；这不解释此前 Active/RDP 时的失败。真实前台聚焦、实体键盘与 mstsc 最小化仍需连接有效会话后验收。本轮未部署线上版本、修改 RDP 设置或向真实桌面发送键鼠。

初次观察租约失败时，本机只读诊断显示输入桌面为 Screen-saver；后续 Release、Debug 及解压包的只读观察租约、窗口枚举和 PNG 截图检查均通过。两次环境前提与结果分开记录。原生输入测试仍使用拦截和模拟会话，真实前台聚焦及键盘验收尚未完成。
