# M4 项目审批与受限执行

日期：2026 年 10 月 10 日。版本：0.1.0-dev.11。本文描述正常 format_version:1 项目的实际行为；旧单实例和 legacy_config 迁移项目保留既有授权语义。开发机证据见 [实现记录](IMPLEMENTATION_STATUS.md)，继续开发见 [M4 交接](HANDOFF_20261010_M4.md)。

## 三种模式与范围

| 模式 | 六个文件工具 | task_start / task_input |
| --- | --- | --- |
| always | 列表、读取、搜索、图像、创建、修改均待批准 | 待批准 |
| workspace_auto | 工作目录按读写许可自动；额外目录和账户其他位置待批准 | 待批准 |
| never | 授予范围内自动；不会增加范围、工具或写权限 | 已启用身份内自动 |

fs_write 的创建仍禁止覆盖；覆盖和 fs_patch 都需要 expected_sha256、独占核验及私有备份，原文和结果为 64 KiB 内 UTF-8，返回 atomic_save:false。审批或 never 不改变这些前置条件。文件内容在待批准时不会读取或返回；目标检查使用物理句柄和对象元数据。修改在读取原文前用钉住的对象再次检查批准版本。

current_user 文件范围可省略 workspace_id，传入绝对本地盘符 path。配置目录的只读与 workspace/额外目录审批类别仍按物理父目录生效。配置范围内的相对路径工具不允许越界；设备路径、流、父目录跳转和不确定物理分类均拒绝。当前账户仍不能读取其系统令牌无权访问的对象。

host_info、host_capabilities 和 tools/list 只返回项目元数据。task_status、task_output 与 task_cancel 校验本项目、原 actor、task_id 和 host_instance_id，沿用原任务授权；输入文本与 EOF 单独审批。

## 查看、批准与重试

pending 在 MCP 结果中使用 isError:true，并带 approval_id、status、tool、project_id、policy_revision、actor、host_instance_id、operation_sha256、target_version、期限与预算；不创建任务或执行文件修改。operation_sha256 是规范参数摘要，不是文件正文摘要。

所有者通过原本机通道查看和处理，MCP 没有批准方法。例如准备一个独立请求文件：

```json
{"command":"inspect","id":"待批准的32位编号"}
```

```powershell
$bot = 'E:/Program/ShellBot/out/x64/Release/WinShellBot.exe'
& $bot --project-owner project-a --request E:/ShellBotSettings/approval.json `
  --manager-data-dir E:/ShellBotProjects/manager/state
```

同一入口支持 inspect、approve、deny、revoke、list；list 不要求 id。inspect 的 text 是原规范请求 JSON，包含准确写入内容、命令 argv 或输入文本/EOF。所有者确认后改用 approve，原调用方再携带相同参数与 approval_id 重试；批准本身不执行操作。HTTP 的 project-mcp 与本机请求的 manager-owner 不能互相消费批准。

许可绑定实际项目、策略版本、工作进程实例、调用者、工具、规范参数、目标对象/版本、期限和总预算。伪造 approved、project_id、不同参数或跨项目 approval_id 均拒绝。配置切换需要新工作进程，旧审批和任务实例失效。撤销只阻止后续消费，不回滚已经产生的结果。

fs_read 单页最多 64 KiB，同一许可总计最多 1 MiB；fs_list 单页最多 200 条，同一许可最多 1000 条。后续页只允许返回的 next_offset，保留原页面大小及范围，可传返回的 encoding；同页重试返回原页，不能跳页。文件或目录对象版本变化需要新批准；许可到期后不能继续读取，即使旧页有缓存。搜索保留原扫描、读取字节与时间预算，图像最多 2 MiB/头部 1600 万像素。超过回执缓存的图像，在原期限和相同物理版本内重新读取并返回相同图像，不重复消耗预算。

写入和修改重试返回同一回执及原备份，不再修改；已完成任务启动重试返回原 task_id、created_new:false。同实例、同 actor、相同参数的文件修改视为同一逻辑操作；要执行不同修改，须使用新的准确内容/原始哈希，不要把旧完成回执当作文件当前状态。task_start 使用 request_key，task_input 使用 input_key，输入精确文本和 EOF 只发送一次。审计故障阻止新操作；状态和取消仍按既有任务归属提供。

统一审批与自动文件修改回执共用 resources.max_approval_records，默认 64，包含完成、拒绝和过期记录；不静默淘汰可能有副作用的回执。自动纯读取不占该队列；never 的命令和输入使用原任务/输入回执预算。记录用尽后先复核结果，再明确重启工作进程；日志与备份保留，内存批准和任务不恢复。命令行请求文件仍限 64 KiB，HTTP 请求限 1 MiB，因此接近 64 KiB 的写入文本需通过 HTTP 提交。

## Windows LPAC 后端

所有者先准备相互独立的普通工作目录、项目 Data、管理器 Data 和程序资源。execution 例子：

```json
{
  "enabled": true,
  "default_identity": "restricted",
  "administrator_allowed": false,
  "runtime_files": ["E:/ShellBotRuntime/native-tool.exe", "E:/ShellBotRuntime/native-helper.dll"]
}
```

这属于可信本机配置，MCP 不能增加资源。最多 16 个精确普通单链接文件，限本地盘符；不授权整个 Windows 目录，不接受管理器或任何项目 Data 内文件。文件为只读/执行资源，整个工作进程期间持有禁止写入/替换的句柄。task_start 只允许其中声明的绝对 .exe，授权后计算程序内容 SHA-256 并绑定任务键；argv 和实际 cwd 也固定。动态脚本和依赖不会仅凭 argv 自动全部固定；必须遵守真实权限与声明资源。

每次启动创建独立 profile/SID 的 LPAC，零 capabilities，并设置 ALL_APPLICATION_PACKAGES_OPT_OUT、禁止 Win32k 系统调用、禁止创建子进程和单进程 Job。父进程核验实际 token 的 AppContainer/SID、无提升/无 capabilities，以及普通通用包组资源拒绝与专用 SID 资源允许的 AccessCheck 对照。开发机 TokenIsLessPrivilegedAppContainer 查询返回 ERROR_INVALID_PARAMETER，程序没有把不支持的查询值当作验收，而采用实际令牌与行为对照。

旁置 ShellBot-SandboxProbe.exe 是产品运行依赖，主工程自动构建；开发用 ShellBot-TestFixture.exe 不属于产品依赖。启动探针执行目录许可、只读、私有日志、注册表、父进程、网络、子进程和 Win32k 检查，失败时命令后端保持不可用，文件工具仍可按策略使用。运行环境仅继承有限允许列表，全部项目 bearer 变量与未知变量被排除。Windows 定义与属性见 [Microsoft AppContainer/LPAC](https://learn.microsoft.com/en-us/windows/win32/secauthz/implementing-an-appcontainer)。

目录启动授权最多 1024 个对象、16 层，拒绝已有重解析点及硬链接文件；读写目录只授予文件访问及删除，不授予改 ACL 或所有者。ACL 变更前写入并刷新受保护恢复日志。停止先杀本人 Job，再移除该实例 SID 的 ACL，保留其他授权及 owner/group，包含新建文件；不跟随后来出现的联接外部目标。强制结束管理器时，下一次显式启动项目先恢复；清理最多 10000 个对象、64 层，失败状态 cleanup_required 阻止继续启动。恢复日志不会恢复批准、任务或副作用。

此初期后端支持兼容上述限制的原生程序，禁用子进程、网络、GUI/Win32k，未增授 registry/COM 等能力。依赖这些能力的 PowerShell、GUI、构建工具或第三方程序可能启动失败；失败不会退回 current_user 或 administrator。LPAC 仍使用 Windows 平台运行资源及自身 profile，不等于只暴露 cwd 的虚拟机，也不抵抗已经授权的普通账户/管理员程序。其他 Windows 版本、独立机器及实际工具兼容性尚需验收；M5 项目管理员和 M6 远程审批/持久恢复未交付。
