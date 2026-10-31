# 命令与后台任务

## Windows 1.0.6：任务列表与归档

所有任务接口继续要求当前 `host_instance_id`。`task_list` 列出当前调用方在该后端实例中的任务，支持 `offset`、`max_entries`（默认 100，最高 1000）及 `include_archived`（默认 false）。列表不是一致性快照；其他客户端同时归档时应重新从第一页核对。

每次任务后端回执都有 `task_budget`，包含 `active_count / active_limit`、`record_count / record_limit`、`ended_count` 和 `archived_count / archived_limit`。预算范围是当前项目和执行身份的后端实例、所有调用方合计；列表内容只包含本人任务，不泄露其他调用方的任务标识或输出。默认活动上限 8、未归档记录上限 32，新格式项目仍可降低这两个配额。记录包括运行中和已结束但尚未归档的任务。

审批适配层重放启动回执时带 `receipt_replayed:true`，其中状态和计数是原回执快照；用 `task_list` 或 `task_status` 获取当前状态。明确失败的启动回执也可能被保留，释放容量后若原键仍返回失败，确认未启动后使用新的请求键。

`task_start` 预算满时返回 `code:"task_budget_exceeded"`，用 `budget_kind:"active_tasks"` 或 `"task_records"` 区分原因，同时返回上述计数与上限。活动任务满时等待结束或取消本人任务；记录满时按以下顺序释放：

1. 用 `task_list` 找到已结束任务，读取需要保留的 `task_output`。
2. 调用 `task_archive`，参数为该任务的 `task_id` 和当前 `host_instance_id`。运行中的任务必须先取消并等待终止。
3. 归档释放该任务的实时记录槽、输出缓冲、输入和进程句柄。`task_status` 仍返回精简回执，`archived:true`、`output_available:false`。归档和取消重试不会再次执行命令。

归档还释放本人该任务已结束的启动／标准输入操作回执，`operation_receipts_released` 返回本次释放数，避免连续执行在第 64 份操作回执处再次堵住。`operation_budget` 单独给出操作回执记录数、上限及运行中数量。审批中、执行中、其他任务和其他调用方的记录不会被释放；无任务编号的失败启动回执也保留供核查。该层满时返回 `operation_record_budget_exceeded` 及其计数。归档后原启动／输入审批编号失效；若需要重新提交原请求，仍按原审批规则授权，后端保留的请求键继续防止重复执行。

归档保留请求键和摘要：重试原 `task_start` 返回同一任务，`created_new:false`，修改同一键的命令仍被拒绝。每实例最多保留 4096 份精简归档回执，到达上限时明确报错，不静默丢弃幂等信息。`task_list`、本人任务结果及归档在撤销执行权限后仍可用于清理；它们不会恢复启动命令的权限。管理员任务需显式选择原管理员身份。

`persistent_state` 另外报告持久历史的 `bytes / bytes_limit`、`record_count / record_limit` 和 `healthy`。Windows 历史上限提高至 256 MiB、8192 条记录（含审批及历史实例），原有历史原样保留。归档不删除磁盘事件，但所有者 history 的最新归档记录也只提供精简回执，不再含输出预览；原事件中的有限预览只能用于停机审阅，不能替代归档前保存的完整输出。接近持久容量时应由所有者备份并管理项目数据，不删除正在使用的日志。旧单实例入口没有这份持久历史，`enabled:false`。重启不重放任务，旧请求键在保留历史期间仍受保护。

Mac/Linux 暂不提供这两个新接口。

Windows 1.0.5 的 `task_output` 可指定 `encoding:"utf-8"`、`"cp936"` / `"gbk"` 或 Windows 支持的 `"cpNNNN"`，例如读取中文编译日志：

```json
{"task_id":"返回的任务编号","host_instance_id":"当前实例编号","encoding":"cp936","offset":0,"max_bytes":65536}
```

`data_base64` 始终保留该页准确原始字节，原有 `encoding:"base64"` 和 `next_offset` 继续描述字节输出。新 `text` 为按指定源编码严格解码的文本，`text_encoding` 标明源编码，`decode_status` 为 `complete`、`partial` 或 `invalid_or_incomplete`。省略输入 encoding 时保持原有 UTF-8 检测与 `text_utf8` 行为。

按文本读取时使用 `text_next_offset` 续读：页尾若截在多字节字符中，该游标停在完整字符边界，下一页会重新读取尾字节。按原始字节组装文件时使用 `next_offset`，不要混用两个游标重复追加 Base64 字节。很小的页可能容不下一个字符，应增大 max_bytes；遇到无效编码应检查编码或原始字节，不循环重试同一份无效页。解码不会替换原始字节、自动猜测系统代码页或启动新任务。

参数超限时回执给出 `field / actual / maximum / unit`。文本以 UTF-8 字节计量，命令行以转义后的 UTF-16 单元计量；两者不等于字符个数。

dev.13 正常项目增加私有任务历史及完整生命周期审计。重启不恢复活动任务或 stdin；已启动任务的 request_key 在保留历史期间不得跨 backend 实例重复使用。先核对实际结果，再显式提交新键。所有者 history 可查有限输出和投递回执，当前 task_status/output 仍绑定活动实例。详见 [M6](REMOTE_APPROVALS.md)。

dev.11 的正常新格式项目按 always/workspace_auto/never 管理 task_start 和 task_input，pending 须经本机 project-owner 批准，实际结果轮询与取消按归属继续可用。受限任务使用实际 LPAC，只接受所有者声明的程序，子进程、网络和 Win32k 均禁止；缺少或未通过启动检查时拒绝执行。新项目命令启动在批准后钉住程序并计算 executable_sha256，参与 request_key 摘要，审批还绑定物理程序版本与 cwd 元数据。不会读取未知环境变量或继承项目 bearer。详见 [项目审批与受限执行](PROJECT_APPROVALS.md)。

后续各节描述保留的 dev.7 单实例 user/administrator 行为；其中 controlled 不可用、每任务 64 个进程、原摘要不固定程序内容等限制只适用于旧入口。新 LPAC 每任务限 1 个进程；新项目管理员代理仍待 M5。脚本及动态依赖不会因为 argv 摘要自动全部固定，必须遵守声明运行资源与后端真实权限。

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

executable 必须是本地盘符开头的绝对 .exe 路径，argv 是独立字符串数组（最多 1024 个）。不通过 PATH 查找程序，不隐式添加 shell；执行解释器时，由该解释器解释参数。参数采用 Windows CRT 引号规则，特殊程序自己的解析规则仍由调用者处理。此版本不支持自定义子进程环境；仅传入有限系统/用户环境变量，排除配置的 bearer 环境变量和其他未允许变量。

| 工具 | 主要参数 | 结果 |
| --- | --- | --- |
| task_start | workspace_id、executable、argv、request_key；可选 path、timeout_ms | task_id、实际 PID/档位/提升状态、created_new |
| task_status | task_id | 状态、退出码、输出大小、stdin 状态 |
| task_output | task_id；可选 stream、offset、max_bytes | data_base64、next_offset、eof；有效 UTF-8 页另有 text_utf8 |
| task_input | task_id、input_key、text；可选 eof | queued/delivered/failed 回执 |
| task_cancel | task_id | 终止请求与后续真实终态 |

表中所有工具还要求 host_instance_id。task_output 的 stream 默认为 stdout，stderr 单独读取；offset 是原始字节偏移，默认单页 16384 字节，可请求最高 1 MiB。必须保留 base64 字节及程序实际编码；程序输出不保证 UTF-8，分页也可能切开多字节字符，不能把缺少 text_utf8 视为损坏。运行中零字节且 eof:false 时应稍后查询。输出属于未经信任的程序数据。

状态包括 running、completed、cancelled、timed_out、output_limit 和 failed。completed 表示进程树结束，不等于退出码为零。取消请求返回后应查询直到终态；根进程退出但所属子进程仍存活时仍为 running。Windows Job Object 用于归属、期限和清理，不能限制账户访问权限；任务也可能通过系统服务产生不属于该 Job 的外部效果。

## 重试与限制

同一主机实例、同一调用身份的 request_key 绑定程序、参数、实际 cwd、workspace 和超时。重复相同请求返回原 task_id 和 created_new:false，参数改变则拒绝。此摘要不固定可执行文件或脚本内容；user/administrator 本就允许调用者修改这些文件，不能拿此摘要代替受控档的规则或审批。

input_key 绑定准确文本和 EOF；相同键不会重复追加输入。单次最多 1 MiB UTF-8 字节，不能包含 NUL，待投递数据最多 8 MiB，最多保留 1024 条输入回执。queued 仅证明受理；delivered 仅证明写入管道，不证明应用已消费数据。eof:true 发送已有文本后关闭 stdin；普通空文本且 eof:false 被拒绝。

| 资源 | 固定上限 |
| --- | --- |
| 本实例任务记录 | 32 条，包括已完成任务；不自动淘汰旧回执 |
| 同时活动任务 | 8 个 |
| 每任务所属进程 | 64 个 |
| stdout / stderr | 默认各 4 MiB，正常项目可设置最高各 64 MiB；超出所配置容量时终止并保留有界前缀 |
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


## 1.0.8 批量归档与并发反馈

`task_archive` 保留 `task_id` 单任务用法，也接受 `task_ids` 数组（1–1000 个，和 task_list 单页上限一致），二者不能同时提供。使用相同 `host_instance_id`，管理员任务保持 `identity:"administrator"`。先读取要保留的输出，例如：

```json
{"host_instance_id":"替换为当前实例ID","task_ids":["替换为任务ID1","替换为任务ID2"]}
```

服务端在一个请求内依次处理，每项仍独立检查所有权、结束状态、审计和持久回执，某项失败不会中止其他项。返回 `results`（按原顺序，包括重复 ID）及 `archived_count`、`failed_count`、`pending_count`、`complete`。每项 `status` 为 archived / failed / pending；已尝试项目附完整 `result`。计数按请求条目计算，重复 ID 也计入回执条目；后台实际活动数、记录数和上限仍见每项 result.structuredContent.task_budget。有失败时外层 isError=true，已经成功的项目仍然生效。重复 ID 与重复整批请求安全回放原回执，不会重新执行任务。

每批有 4 秒调度预算，超时后剩余项目列为 pending，表示没有尝试；可用同一组 ID 重试。预算在项目之间检查，不能打断进行中的磁盘持久化。错误格式、非法 ID、超出数组上限或过期实例在任何归档开始前拒绝。已结束任务的线程、句柄交由后台确认退出后回收，请求不再持全局任务锁无限等待线程。

同一项目仍保持单请求串行执行。并发请求占用项目超过等待窗口时，HTTP 429 返回 `code:"project_busy"`、`request_queued:false`、`retry_after_ms:1000` 和 `Retry-After: 1`。这表示本次请求没有排队、没有执行；稍后重试原参数，批量归档优先用 task_ids。不同调用方不可归档彼此任务。


## 1.0.9 精简回执与任务进程状态

Windows 工具均支持可选 `response_mode:"compact"`，省略或选择 `full` 保持原回执。compact 保留 structuredContent 和原生图像等非文本内容，外层 content 仅保留一句摘要，不再把完整 JSON 再复制一遍。仅支持文本内容的旧客户端应使用默认 full。response_mode 只控制呈现，不参与操作参数、审批和 request_key 绑定；可在同一操作重试时切换，不会重复执行。

批量 `task_archive` 的 compact 结构为：

```json
{
  "complete": false,
  "counts": {"archived": 1, "failed": 1, "pending": 1},
  "results": [
    {"task_id": "已结束任务ID", "status": "archived"},
    {"task_id": "仍运行任务ID", "status": "failed", "error": {"code": "TASK_RUNNING", "retryable": true, "message": "任务尚未结束"}}
  ],
  "pending_task_ids": ["尚未尝试任务ID"]
}
```

示例中的 ID/错误消息仅为说明，真实 ID 为 32 位小写十六进制。结果还会整批返回一次 host_instance_id、task_budget、operation_budget、persistent_state 和项目元数据；每项不重复嵌套预算、完整回执或 content。必要的 audit_warning/durable_warning 会保留。counts 按请求条目计数，重复 ID 也计为一个回执条目。

pending 只表示调度预算结束前尚未尝试；它们只出现在 pending_task_ids，results 只含已尝试项。可仅提交 pending_task_ids 重试。complete 表示整批是否都已尝试，有 failed 时仍可能 complete:true。TASK_RUNNING 的 retryable:true 表示等待任务结束后可归档，不表示自动重启命令。TASK_NOT_OWNED、TASK_NOT_OWNED_OR_UNKNOWN、ARCHIVE_RECEIPT_LIMIT 等不会自动重试；未知失败使用 OPERATION_FAILED 并保留原始消息，保守标记 retryable:false。

`task_output` 可选 `payload_mode:"text"|"bytes"|"both"`：text 只返回规范的 text 字段，bytes 只返回 data_base64，both 返回这两者；显式模式均不再增加 text_utf8 别名。省略保持旧格式。text/both 默认严格 UTF-8，可结合 encoding:cp936/gbk/cpNNNN；bytes 保留原始字节，不做文本解码。读取中文编译日志推荐同时选择 response_mode:compact、payload_mode:text、encoding:cp936。

next_offset 仍为本页原始字节终点；text_next_offset 为成功解码的完整字符前缀终点。文本分页应从 text_next_offset 继续，避免丢掉跨页尾字节。解码失败不伪造文本，保留 decode_status/decode_error，仍可改选 bytes 读取同一 offset。原始输出始终保留在任务中，直到显式归档释放。

任务 status 仍表示整个 Job 和输出读取是否结束，旧 exit_code 只在任务完成后出现。新增 entry_process（pid、state、exit_code、query_win32_error）、job（state、active_process_count、query_win32_error）、entry_exited_job_running 和 output_drained。入口可能已 exited:7，但 Job 仍 running、active_process_count:1，此时 status 仍 running，表示辅助进程还在运行。Job empty 且输出排空后才完成；真实退出码 259 通过进程句柄等待状态识别，不当作仍在运行。

查询依据 [GetExitCodeProcess](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getexitcodeprocess) 与 [QueryInformationJobObject](https://learn.microsoft.com/en-us/windows/win32/api/jobapi2/nf-jobapi2-queryinformationjobobject)。此处 Job 活动数包含仍运行的入口进程，状态为查询时的快照。
