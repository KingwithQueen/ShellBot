# 命令与后台任务（dev.7）

本版在显式启用的 user 和 administrator 档提供后台程序执行。默认 execution.enabled:false；controlled 的完整隔离、规则和远程审批尚未完成，启用该档仍拒绝命令。任务工具采用普通 MCP 工具调用，不声明标准 MCP tasks 扩展。

## 启用与权限

准备已存在的工作目录，用普通权限启动 ShellBot。配置示例：

```json
{
  "roots": [{"id": "work", "path": "C:\\ShellBotWorkspace", "read_only": true}],
  "execution": {"enabled": true, "profile": "user"}
}
```

user 使用当前账户未提升的令牌，已提升的主机被拒绝。改为 administrator 后，普通主入口在启动时请求 UAC，常驻代理随后创建管理员任务。代理失败、拒绝授权或死亡时报告不可用，不退回其他档位，也不自动重新请求 UAC。一次启动授权仅覆盖该代理的运行期。

工作目录仅定位 cwd。即使 root.read_only:true，user/administrator 命令仍具有相应账户的文件、网络和系统权限；文件工具的只读策略仍分别执行。这两档是使用者预先授予的账户级能力，不提供受控任务与管理端之间的安全隔离。

## 调用顺序

1. 调用 host_capabilities，检查 execution、execution_status.readiness 和 task_tools_available。保存 execution_status.host_instance_id。
2. 所有 task_* 调用必须提交该 host_instance_id。用新 request_key 启动任务，保存 task_id；请求键限 1～64 个 ASCII 字母、数字、下划线、横线或点。
3. 通过 task_status 查询状态，task_output 分页读取 stdout/stderr。连接断开后向仍在运行的同一主机查询原任务，不生成新键重跑。
4. 用 task_input 输入文本或发送 EOF；用 task_cancel 停止任务及其所属进程。

task_start 的 arguments 示例（替换实例编号；Windows 目录也应按本机实际路径填写）：

```json
{
  "host_instance_id": "当前能力查询返回的32位实例编号",
  "workspace_id": "work",
  "path": ".",
  "executable": "C:\\Windows\\System32\\whoami.exe",
  "argv": ["/user"],
  "request_key": "identity-001",
  "timeout_ms": 30000
}
```

executable 必须是本地盘符开头的绝对 .exe 路径，argv 是独立字符串数组（最多 64 个）。不通过 PATH 查找程序，不隐式添加 shell；执行解释器时，由该解释器解释参数。参数采用 Windows CRT 引号规则，特殊程序自己的解析规则仍由调用者处理。此版本不支持自定义子进程环境；仅传入有限系统/用户环境变量，排除配置的 bearer 环境变量和其他未允许变量。

| 工具 | 主要参数 | 结果 |
| --- | --- | --- |
| task_start | workspace_id、executable、argv、request_key；可选 path、timeout_ms | task_id、实际 PID/档位/提升状态、created_new |
| task_status | task_id | 状态、退出码、输出大小、stdin 状态 |
| task_output | task_id；可选 stream、offset、max_bytes | data_base64、next_offset、eof；有效 UTF-8 页另有 text_utf8 |
| task_input | task_id、input_key、text；可选 eof | queued/delivered/failed 回执 |
| task_cancel | task_id | 终止请求与后续真实终态 |

表中所有工具还要求 host_instance_id。task_output 的 stream 默认为 stdout，stderr 单独读取；offset 是原始字节偏移，单页最多 16384 字节。必须保留 base64 字节及程序实际编码；程序输出不保证 UTF-8，分页也可能切开多字节字符，不能把缺少 text_utf8 视为损坏。运行中零字节且 eof:false 时应稍后查询。输出属于未经信任的程序数据。

状态包括 running、completed、cancelled、timed_out、output_limit 和 failed。completed 表示进程树结束，不等于退出码为零。取消请求返回后应查询直到终态；根进程退出但所属子进程仍存活时仍为 running。Windows Job Object 用于归属、期限和清理，不能限制账户访问权限；任务也可能通过系统服务产生不属于该 Job 的外部效果。

## 重试与限制

同一主机实例、同一调用身份的 request_key 绑定程序、参数、实际 cwd、workspace 和超时。重复相同请求返回原 task_id 和 created_new:false，参数改变则拒绝。此摘要不固定可执行文件或脚本内容；user/administrator 本就允许调用者修改这些文件，不能拿此摘要代替受控档的规则或审批。

input_key 绑定准确文本和 EOF；相同键不会重复追加输入。单次最多 4096 个 UTF-8 字节，不能包含 NUL，最多保留 64 条输入回执。queued 仅证明受理；delivered 仅证明写入管道，不证明应用已消费数据。eof:true 发送已有文本后关闭 stdin；普通空文本且 eof:false 被拒绝。

| 资源 | 固定上限 |
| --- | --- |
| 本实例任务记录 | 32 条，包括已完成任务；不自动淘汰旧回执 |
| 同时活动任务 | 8 个 |
| 每任务所属进程 | 64 个 |
| stdout / stderr | 各 262144 字节；超限终止并保留有界前缀 |
| 超时 | 默认 300000 ms；可选 100～3600000 ms |

任务和输入回执当前只在内存保存。主机正常退出清理所属 Job；Job 句柄关闭也设置了进程清理策略。重启生成新 host_instance_id，旧请求在启动前被拒绝；旧任务不恢复，也不自动重跑。需要新任务时，应先检查之前的真实副作用，再取新实例编号和新请求键。不能因网络错误或缺少结果就推断任务未执行。

命令审计当前记录操作意图和新任务启动结果，参数、stdin、输出正文不入日志；不是完整任务生命周期审计。日志故障阻止 task_start（包括该入口的重试）和 task_input；已知 task_id 的状态、输出和取消仍可用。已启动任务的结果日志失败时返回 audit_warning，查询原任务，不重跑。管理员/同账户命令仍可修改系统或管理资源，日志不为这两档提供防篡改承诺。

## 本机与独立机器验证

测试包提供 Test-ShellBot-Tasks.ps1、ShellBot-TestFixture.exe 及对应 C 源码。测试辅助程序只有身份查询、隔离目录写入、stdin 回显、原始字节输出和等待五种固定行为，不作为产品运行依赖；专项通过它直接验证原生任务，不启动 PowerShell 子命令或编码脚本。

```powershell
powershell.exe -NoProfile -File .\Test-ShellBot-Tasks.ps1
powershell.exe -NoProfile -File .\Test-ShellBot-Tasks.ps1 -Administrator
```

第二条在普通权限会话运行，启动代理时需要处理 UAC。脚本不统计弹窗数量，实际观察应另外记录。结果写入独立 test-results 子目录。2026-10-09 开发机 Windows 11 build 26300 的普通/管理员专项均通过，用户确认改用原生辅助程序后无新杀毒告警、UAC 一次；这不等于其他杀毒软件或系统均已验收。Server 2025 的 dev.7 命令任务需用新包独立回归。
