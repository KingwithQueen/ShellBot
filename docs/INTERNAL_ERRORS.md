# 1.1.1 可见诊断与返回链路

有效的 tools/call 内部故障使用 HTTP 200 和标准 MCP result：isError:true、简短 content、structuredContent 中的 error_code/request_id/trace_id/host_instance_id/stage/execution_state/retry_guidance。不再只依赖 JSON-RPC error.data；部分客户端会把它转换为泛化的 -32603 并丢弃诊断。2026 协议的工具失败仍有 resultType:complete 和 serverInfo。无效协议、解析和认证请求仍按原有 HTTP/JSON-RPC 错误返回。

request_trace 属于每次调用的诊断，不进入审批摘要或持久幂等回执；重试时关联编号不同，任务 ID、文件版本和原操作结果仍按原规则复用。

普通工具结果在 structuredContent.request_trace 中携带关联信息，compact 文本也含 request_id。每个 HTTP 请求由服务端生成独立 X-ShellBot-Request-ID，贯穿管理器、工作进程和响应；忽略客户端伪造的同名头。早期 HTTP 拒绝也含关联编号、阶段和 not_started。已认证的项目忙/停止工具请求返回 PROJECT_BUSY/PROJECT_UNAVAILABLE 标准工具错误；忙请求 request_queued:false 且 retry_after_ms:1000，停止项目必须由所有者明确启动。

execution_state 的 result_available 只说明工作进程已生成工具回执，不证明客户端收到、文件修改一定成功或后台任务已退出；应继续检查工具本身的 isError、文件回执及 entry_process/job。not_started 表示请求未调用；unknown 表示副作用不能确认，服务端不会自动重放。有效截图或新观察凭据也不保证 Windows 授予前台权限。

调用方如果只收到客户端 Internal error，可调用 host_capabilities（现有参数即可）检查 request_diagnostics。默认显示最近 8 条；刷新工具定义后可指定 request_diagnostics_limit:128。工作进程共享的 128 条内存窗口仅向当前项目、当前传输调用方返回匹配记录，满后淘汰旧诊断，不占任务预算。记录时间、阶段、工具、耗时、结果状态与键的 SHA256，不存正文或认证。通过 request_key/input_key 的 SHA256 可关联反馈记录；没有键的文件调用可按时间和工具名定位。重启后内存记录消失，内部错误继续记录受保护审计日志。记录显示 result_available 而客户端报错，可作为检查回执之后链路的线索，不能单凭此断言网络原因。

原反馈机器的具体间歇原因尚未复现。升级后若仍发生，请保留可见关联编号、host_capabilities 的最近诊断和原始时间；它们不会授权重放桌面输入。此前 1.1.0 的接口说明与历史 JSON-RPC 错误形式如下。

# 内部错误与请求关联

Windows 1.1.0 为工具响应增加 `_meta["shellbot/request_id"]`。这是服务端生成的单次调用关联编号；即使调用方重复使用同一 JSON-RPC `id`，关联编号也不同。它不参与审批、任务幂等或权限判断。`response_mode:"compact"` 仍只在 `content` 中返回一句摘要，诊断字段不会复制整个结果。

工具返回空值、返回类型异常、响应序列化失败/超限，或工作进程失联时，返回 JSON-RPC `-32603`，原请求 `id` 保留在外层：

```json
{
  "jsonrpc": "2.0",
  "id": "client-id",
  "error": {
    "code": -32603,
    "message": "Internal error; inspect error.data and correlate the server diagnostic.",
    "data": {
      "error_code": "INTERNAL_ERROR",
      "request_id": "server-generated-32-hex-id",
      "host_instance_id": "project-worker-instance",
      "stage": "worker_reply",
      "execution_state": "unknown",
      "retry_guidance": "same_key_only",
      "reason": "worker_disconnected_deadline_or_invalid_reply"
    }
  }
}
```

`stage` 表示发现问题的边界，不能据此断言副作用发生的位置：`parse` 是请求解析和初始响应分配，`authorize` 是协议/项目绑定与参数校验，`worker_invoke` 是调用准备，`worker_reply` 是工作进程通信或工具返回，`serialize` 是返回结果编码。普通参数和文件错误仍保留原有错误语义；缺少的 `AGENTS.md` 应返回文件工具错误。

`execution_state:"not_started"` 只用于确定尚未调用的情况。调用已开始、通信中断或生成回执失败时使用 `unknown`；服务端不会自动重放。重试指引为：

| retry_guidance | 操作 |
| --- | --- |
| retry_original_request | 调用尚未开始，可重试原请求 |
| same_key_only | task_start 保留原 request_key，task_input 保留原 input_key，参数不变；不得换键重新执行 |
| observe_before_retry_no_automatic_replay | 先重新观察桌面/前台窗口和结果；不要自动重放键鼠、粘贴、聚焦等动作 |
| inspect_state_before_retry | 先检查实际状态，再决定是否重试；缺少回执不证明没有执行 |

管理器若在清理前确认工作进程已经退出，reason 会记录 `worker_exited_0xXXXXXXXX`，使用进程句柄读取的实际退出码；仍存活或无法确认时保留通信/超时/异常返回的原因范围，不猜测崩溃。

工作进程通信失效后，管理器回收该项目的工作进程，不会为此次请求自动重启或重新派发。只有所有者明确启动才创建新实例。进程退出后旧实例的幂等记录可能已不可查询，因此 `same_key_only` 不承诺跨重启恰好一次执行。

受保护的 `audit-*.jsonl` 中记录 `internal_error`，可按 request_id 关联 host_instance_id、tool、stage、reason、参数类型计数、进程/线程编号和诊断边界的栈地址。不会记录完整参数、命令行、输入正文或认证令牌。日志中的 `stack_origin:"diagnostic_boundary_not_exception_site"` 明确表示这是发现异常返回时的调用栈；工作进程已经崩溃时，管理器的栈不能替代崩溃现场转储，也不能据此猜测崩溃原因。严重内存耗尽仍可能无法分配错误回执；日志不可写时尝试向 stderr 输出同样的脱敏诊断。

本次复现覆盖受控错误与进程中断，未在原反馈机器复现偶发 `-32603`。升级后若继续出现，请保留客户端错误、关联编号及相应服务端日志，再定位具体原因。
