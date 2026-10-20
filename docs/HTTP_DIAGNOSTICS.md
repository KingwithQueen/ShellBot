# ShellBot HTTP 对照诊断

2026-10-20 收尾：普通环境已复现旧 10054，Windows 关闭前增加有界对端排空后，按当前程序分别重跑 Debug/Release 的五组各 1000 次 TCP 对照。明确区分建连超时和响应重置，记录 30 秒建连/10 秒响应预算及最大耗时，无应用层重试；HTTP/1.0、组合 close 令牌、半关闭、超限、分块拒绝和真实连接复用另有 221 项报告。最终结果及历史失败见 [M13 交接](HANDOFF_20261020_M13.md)和 build/validation/m13-local-acceptance.json；下方受限环境及旧系统结果保留历史。

2026-10-20：dev.20 补充响应短写/写入失败处理，并将 Connection: close、HTTP/1.0 和错误响应同步到实际关闭状态。响应帧故障注入 25 项通过；当前执行环境的真实回环连接返回 WinError 10013，不能用此组件结果宣布历史 10054 修复。新版回环对照为本地 tests/http_close_checks.py，覆盖普通新连接、close、分段请求、拒绝响应与并发，失败不重试。应在普通 Windows 执行环境中比较旧/新程序；原失败记录保留。

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
