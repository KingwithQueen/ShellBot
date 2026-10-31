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
