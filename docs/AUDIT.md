# 审批、文件与任务操作审计

dev.13 正常项目的当前行为见 [M6 记录与恢复](REMOTE_APPROVALS.md)：文件调用记录意图及完成/阻断，任务记录自然完成、取消、期限、输出限额和输入投递。决策 actor 可为 local-owner 或 remote-owner。完整参数及有限输出只进入独立私有 state，不进入审计。下文保留旧入口和历史说明，不作为正常项目当前能力限制。

日期：2026 年 10 月 9 日。引入版本：`0.1.0-dev.3`；dev.5 增加覆盖、局部替换与备份事件，dev.7 增加任务操作意图和新任务启动结果。日志不是全部调用或完整任务生命周期记录。

本版把审批状态、文件创建和修改结果保存为 UTF-8 JSON Lines。每实例在 Data 创建独立 audit-实例编号.jsonl，退出保留；重启不追加、修复或删除旧文件，不恢复审批。

## 记录内容

记录包含格式版本、程序版本、实例编号、实例内递增序号、UTC Unix 毫秒时间、事件和调用身份。审批相关事件还包含审批编号、工具名、workspace_id、规范目标、拟写入内容的 SHA-256 与字节数。

事件包括 instance_opened、instance_closed、approval_requested、approval_approved、approval_denied、approval_revoked、approval_expired、write_started、backup_saved、write_completed 和 write_failed。修改事件增加 operation、original_sha256，备份后增加 backup_id；backup_saved 刷新成功后才修改原文件。完整原文只进入独立私有备份，不进入审计正文，见 FILE_EDIT.md。过期按请求检查记录；普通读取和原始 HTTP 交互尚不在审计范围。

调用身份由传输设置。所有者决策记录 local-owner，与模型提供的 clientInfo 无关。日志不保存文件正文、令牌、完整协议消息或原始异常文本；目标路径本身仍可能包含用户信息，因此日志属于私有运行数据。

## 存储与失败行为

Windows 相对已固定的数据目录句柄创建新文件，拒绝打开既有同名文件。日志使用禁止继承的 ACL，只授予当前账户与 SYSTEM；打开期间允许读取，拒绝其他写入与删除共享。每条事件写入后调用 FlushFileBuffers，成功后才确认需要审计的批准或开始执行。

`audit_max_bytes` 默认 8 MiB，可设为 4096～67108864，限定单个运行实例的日志。单条事件也有 256 KiB 上限。此版不自动轮转或删除旧文件，历次实例的累计占用需要所有者在主机停止后自行归档管理。

| 情况 | 行为 |
| --- | --- |
| 启动时不能创建受保护日志或刷新首条记录 | 启动失败，未开始接收 MCP 或审批请求 |
| 运行时日志写入失败或达到预算 | 本实例停止继续记录，阻止新审批、批准、新文件执行、task_start 和 task_input；状态/输出读取与 task_cancel 仍可用，能力查询显示 audit_healthy:false |
| 日志故障时拒绝、撤销或到期 | 仍然使许可失效，返回实际状态及 audit_warning |
| write_started 无法写入 | 不开始文件创建，原文件状态不变 |
| 文件已提交但结果事件写入失败 | 返回 completed 及 audit_warning，保留真实文件；重试查询原结果，不再次执行 |

健康状态变为 false 后，即使磁盘恢复，本实例也不自动继续写入可能存在半条记录的日志。所有者确认问题后停止、重新启动；旧审批不会恢复，已创建的文件不会自动回滚。只读工具和本机查询仍可使用。

崩溃可能留下末尾不完整的日志行，或者只有 write_started 没有结果记录。不能从缺少结果推断文件未创建；应检查实际目标。此版没有跨文件事务、审计索引或重启任务恢复。管理员及同账户程序仍属于本机信任范围，日志不提供对它们的防篡改证明。

host_capabilities 返回 durable_audit:true、audit_healthy、audit_scope 和 audit_max_bytes；durable_approvals 仍为 false。

平台实现参考：[相对句柄创建文件](https://learn.microsoft.com/en-us/windows/win32/api/winternl/nf-winternl-ntcreatefile)、[刷新文件缓冲区](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers)、[受保护的 ACL](https://learn.microsoft.com/en-us/windows/win32/secauthz/security-descriptor-string-format)。

## dev.7 任务事件

task_operation_requested 在 start/input/cancel 的后端操作前记录意图；task_started 仅对 created_new:true 的成功启动记录一次。字段包括真实传输 actor、请求键或任务编号、配置档位、规范请求摘要、操作及启动 PID；任务参数、stdin 和输出正文不入日志。

意图不证明实际启动、输入消费或取消完成。此版没有任务自然完成、超时、输出消费或每次输入投递的完整事件链，不能据现有日志重建所有任务状态。输入正文摘要也不代替完整审批绑定。

无法刷新操作意图时拒绝 start/input，包括经 task_start 的重试；取消仍继续尝试，以便清理已经运行的任务。新任务已启动但 task_started 记录失败时返回 audit_warning，不把任务当成未执行；通过已知 task_id 查询，不生成新请求键重跑。受信任的同账户或管理员命令可能修改管理资源，日志不提供针对它们的防篡改证明。
