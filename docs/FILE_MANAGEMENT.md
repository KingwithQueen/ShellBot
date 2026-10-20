# 原生文件管理（dev.20 / M13）

正常新格式项目可显式启用 fs_stat、fs_mkdir、fs_copy、fs_move、fs_delete；不要求命令或桌面权限。旧单实例和 legacy_config 不开放这些工具。原工具索引不变，许可扩展为 64 位，共 37 个工具；旧配置不自动开放新增工具。

| 工具 | 参数及行为 |
| --- | --- |
| fs_stat | workspace_id、path，可选 directory:true；返回 type、size、version，不读取正文 |
| fs_mkdir | workspace_id、path、request_key；在已有父目录中创建一层目录，目标必须不存在 |
| fs_copy | workspace_id、path、destination_path、expected_version、request_key；复制最多 64 MiB 的单个二进制文件，不覆盖目标 |
| fs_move | 同 fs_copy，可选 directory:true；同一卷内重命名或移动文件/目录，不覆盖目标 |
| fs_delete | workspace_id、path、expected_version、request_key，可选 directory:true；文件删除前保存私有备份，目录必须为空 |

复制和移动可用 destination_workspace_id 指定另一授权目录，省略时沿用源 workspace_id。显式 current_user 文件范围可省略 workspace_id，使用绝对本地盘符路径；目标独立判定权限。identity:administrator 仍须项目许可和已核验共享代理，普通批准不用于管理员后端。

先通过 fs_stat 查询源，再将返回的 version 作为 expected_version：

```json
{"name":"fs_copy","arguments":{"workspace_id":"work","path":"report.pdf","destination_path":"archive/report.pdf","expected_version":"上一步返回的64位version","request_key":"copy-report-1"}}
```

version 是对象身份和元数据版本，不是内容 SHA-256。源替换、内容或相关元数据变化后拒绝旧版本。目录操作明确 directory:true；不能操作授权根、重解析对象或多硬链接文件。

always 中 fs_stat 也先审批；workspace_auto 对工作目录自动、额外目录审批；never 仍受工具、范围、只读和身份限制。复制源可只读，目标须可写；移动两处均须可写。请求绑定目录对象、参数和版本，执行时固定源及父目录句柄，不通过 shell 或退回字符串路径覆盖/删除。

request_key 为 1–64 个字母、数字、下划线、点或连字符。重试保持原参数、键及 approval_id；相同完成请求返回原回执，不再次操作。更改参数须新键并重新审阅；重启不恢复旧编号或许可，也不自动重放。回执描述原结果，不代表目标当前状态。

删除文件最多 64 MiB，备份为所属后端私有 Data/deleted-<backup_id>.bin，仅当前账户和 SYSTEM 可访问。备份和 backup_saved 审计先刷新成功后才删除；失败保留源。backup_id 也进入持久记录。备份保存字节，不包含全部原 ACL，不是 Windows 回收站。所有者核对后明确恢复，程序不自动恢复或清理历史；与文本修改共用每实例 64 个备份槽。

组件测试覆盖真实字节、Unicode、只读源复制、禁止覆盖、同目录文件/目录重命名、删除备份、审批、版本和重试。跨目录移动保留原生目标目录句柄；先前 AppContainer 环境返回 STATUS_INVALID_HANDLE 的报告保留，收尾已通过普通 owner/worker 及真实管理员代理的跨目录移动验证。最终程序和报告以 M13 收据为准。跨卷移动、目录树复制和递归删除未纳入接口，须显式拆分操作。原生重命名的同目录简单名称及目录句柄语义参考 [Microsoft FILE_RENAME_INFORMATION](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_file_rename_information)。
