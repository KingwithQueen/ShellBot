# ShellBot HTTP 对照诊断

日期：2026 年 10 月 6 日。

Server 2025 上 dev.2 的首次原脚本反馈 `Core: passed`、关闭连接 2/100 失败；随后原脚本复测关闭连接 0/100，新增 HTTP-only 诊断整体通过，close 和 fresh keep-alive 各 0/100 失败。当前开发机仍有失败，本次通过不视为根因已修复。

独立诊断包只包含脚本，必须与 manifest.json 的 supported_version 对应的程序一起使用。旧诊断包保留；当前仓库脚本面向 dev.5，不替换旧版脚本。此诊断不要求重复文件审批测试，也不检查真实域名。

将诊断包中的 `Test-ShellBot-Http.ps1` 复制到已有 `WinShellBot.exe` 的目录，在该目录执行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Test-ShellBot-Http.ps1 -HttpOnly
```

默认执行 100 次持续连接查询，以及两组匹配的对照请求：100 次新建 keep-alive 连接、100 次新建 Connection: close 连接。两组都调用只读 ping，失败后不重试。脚本只监听本机随机端口，使用新的隔离测试目录，完成后停止它自己启动的主机。无需管理员权限或其他软件。

只测 HTTP 时，`core_status: not_run` 表示本次跳过已验证的文件与审批检查，不表示这些功能失败。`http_basic_status` 只覆盖基本 HTTP、令牌和持续连接检查；完整结果以顶层 `status` 为准，任一对照诊断失败都会返回 `needs_investigation` 和非零退出码。

新报告包含：

- 系统名称和实际 build、程序版本和 SHA-256。
- 两组的计划调用数、实际完成调用数、失败数。
- 每个失败请求的阶段：连接、发送、响应头、响应正文或 JSON 解析。
- 已接收字节数、耗时、异常类型链、Windows socket 错误码（如有）。

报告不记录 bearer 令牌、请求头、响应正文或失败请求的任意异常消息。请回传新 `test-results/时间-随机标识/report.json` 的完整内容或文件。原来的报告保留，可一并回传，以核对此前的系统版本和程序校验值。

诊断仍不是公网 MCP、OAuth 或 cloudflared 验收。当前开发机已定位到响应头尚未到达时出现 10054；Server 2025 的成功反馈为控制台输出，完整报告仍待回传，不能推定此前失败具有相同原因。
