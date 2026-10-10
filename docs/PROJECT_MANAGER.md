# 项目管理器与工作进程

更新日期：2026 年 10 月 10 日。程序版本：`0.1.0-dev.10`。M2 提供普通权限管理器、每项目独立工作进程和本机所有者命令行入口；M3 项目 HTTP 路由和 bearer 认证已接入，见 [项目网关](PROJECT_GATEWAY.md)。

## 创建与配置

`--project-config` 接受一份单项目 JSON 对象，其结构等同于 [项目配置](PROJECT_CONFIGURATION.md) 中 projects 数组的一个元素。项目 id 必须与命令参数一致。现有注册配置可通过 `--projects` 引用。

```powershell
$bot = 'E:/Program/ShellBot/out/x64/Release/WinShellBot.exe'
& $bot --project-create project-a --project-config E:/ShellBotSettings/project-a.json `
  --output E:/ShellBotSettings/projects.v1.json
& $bot --project-create project-b --project-config E:/ShellBotSettings/project-b.json `
  --projects E:/ShellBotSettings/projects.v1.json --output E:/ShellBotSettings/projects.v2.json
& $bot --project-configure project-a --project-config E:/ShellBotSettings/project-a-updated.json `
  --projects E:/ShellBotSettings/projects.v2.json --output E:/ShellBotSettings/projects.v3.json
```

输出父目录须已存在。输出和 `.source-backup.json` 都必须是新文件；原文件不会被覆盖。加入项目时拒绝已有 id，配置项目时拒绝不存在的 id。配置更新将原 policy_revision 加一，不采信定义文件中的替代版本号；uint64 用尽时拒绝更新。写入前对完整候选注册配置校验，备份保存一次读取的原字节，并使用当前账户和 SYSTEM 的私有 ACL。首次创建的备份保存单项目定义，其后保存原注册配置。

这些命令不创建 Data、不启动工作进程、不请求 UAC。管理器使用启动时的不可变配置快照；写入新文件后，需要明确停止管理器，再以新文件启动。该阶段不提供在线重新加载、项目删除或配置原地覆盖。

## 启动、查询和停止

管理器必须由普通、未提升的账户启动；已提升、低完整性、受限或 AppContainer 入口会被拒绝。管理器与工作进程均不启动管理员代理。

```powershell
& $bot --manager --projects E:/ShellBotSettings/projects.v2.json `
  --manager-data-dir E:/ShellBotProjects/manager/state
```

管理器启动后输出 `manager_ready:true` 并持续运行，注册项目初始为 stopped。另一个普通账户终端可执行：

```powershell
& $bot --project-start project-a --manager-data-dir E:/ShellBotProjects/manager/state
& $bot --project-start project-b --manager-data-dir E:/ShellBotProjects/manager/state
& $bot --project-status --manager-data-dir E:/ShellBotProjects/manager/state
& $bot --project-status project-a --manager-data-dir E:/ShellBotProjects/manager/state
& $bot --project-stop project-a --manager-data-dir E:/ShellBotProjects/manager/state
& $bot --manager-stop --manager-data-dir E:/ShellBotProjects/manager/state
```

管理器持有本人进程和 Job 句柄。停止项目只终止该项目工作进程及任务树；停止管理器清理它创建的所有项目。管理器被强制结束时，Job 的关闭清理也会终止其工作进程与后代。不会按程序名或外部 PID 搜索后终止无关进程。

每个工作进程拥有独立 Data 实例锁、任务空间、幂等记录、审批队列、备份和审计。Data 创建后用实际目录句柄、卷及文件标识检查别名与重叠；本地卷 GUID 路径进一步处理不同盘符或挂载点。管理器固定 Data 对象身份，工作进程在创建锁和审计前复核，路径重新指向另一对象时拒绝启动。配置目录范围的项目不能把管理器或任一项目 Data 纳入目录授权；普通账户完整权限项目仍属于账户级信任范围。

工作目录与额外目录各最多 16 个；工作进程最多绑定 32 个目录，启动时拒绝物理别名重叠。原单实例仍限 16 个根目录。

## 本机请求入口

本机管理使用受保护的所有者通道，核验实际连接令牌、账户及受限身份，不接受 MCP bearer 凭据。管理器省略 --http 时不监听 HTTP；启用 --http 后，项目 MCP 仍不能调用管理入口。管理员共享授权和远程审批尚未接入。

```powershell
& $bot --project-request project-a --request E:/ShellBotSettings/request.json `
  --manager-data-dir E:/ShellBotProjects/manager/state
```

request.json 是一个 JSON-RPC 对象。例如使用逐请求协议读取当前项目能力：

```json
{
  "jsonrpc": "2.0", "id": 1, "method": "tools/call",
  "params": {
    "name": "host_capabilities", "arguments": {},
    "_meta": {
      "io.modelcontextprotocol/protocolVersion": "2026-07-28",
      "io.modelcontextprotocol/clientCapabilities": {}
    }
  }
}
```

返回对象包含 project_id 和 response。旧初始化协议也可在同一工作进程内依序使用，不能混用协议时代。命令行请求文件当前限 64 KiB；通道请求限 1 MiB，响应限 4 MiB。每个工作进程一次处理一个请求，管理器有固定四个所有者服务线程；同项目正在处理请求时返回 busy，客户端可在完成后重试管理查询，不自动重发有副作用的请求。单次工作进程通信共用 8 秒期限，超时或断线后终止本人工作进程并标记失败，明确提示复核可能已发生的副作用。

工作进程只接收本项目快照，固定项目和策略版本；本机请求 actor 为 manager-owner，M3 网关请求 actor 为 project-mcp，两者任务归属独立。模型参数无法切换项目或 actor；任务归属由实际后端查询，host_instance_id 还必须匹配当前工作进程。重启后实例和回执空间改变，旧请求会被拒绝。管理员项目配置不会转成普通命令执行。

配置只保存独立环境变量引用。启用 M3 HTTP 时，管理器读取并核验启用项目的独立令牌；凭据不进入工作进程通信。启动工作进程的环境采用固定允许列表，排除全部注册项目的凭据变量及未知变量；任务也不会继承这些凭据。退出时清除管理器令牌缓冲。

## 配额和功能边界

普通新项目可包含可选 resources 对象：

| 字段 | 默认值 | 允许范围 |
| --- | --- | --- |
| max_active_tasks | 8 | 1～8 |
| max_task_records | 32 | 1～32，至少等于 max_active_tasks |
| max_output_bytes | 262144 | 1024～262144，每个输出流分别计数 |
| max_timeout_ms | 3600000 | 100～3600000 |
| max_approval_records | 64 | 1～64，含等待到期的回执 |
| worker_memory_mb | 512 | 16～4096，整个工作进程 Job 与任务树合计 |

任务超过并发或记录预算会被拒绝；已有幂等回执不会静默淘汰。未指定任务期限时使用 300000 毫秒与项目上限中的较小值。输出溢出和超时仅终止本任务 Job。备份沿用每实例 64 份上限，审计沿用项目 audit_max_bytes；持久回执、跨重启累计保存预算和恢复属于 M6。

M2 实际可执行的范围：

| 操作 | 当前行为 |
| --- | --- |
| 主机信息、能力与工具列表 | 固定项目上下文，只列本项目启用的工具；已有任务结果和取消保留归属校验 |
| never 的已配置目录读取 | 实际句柄检查后可列表、读文本、搜索和查看图像 |
| workspace_auto 的工作目录读取 | 自动允许；额外目录仍因需要 M4 审批而拒绝读取正文 |
| always 的文件访问 | 返回 project_approval_adapter_unavailable，不读取正文或返回搜索摘要 |
| 普通命令与输入 | 仅允许统一策略通过且无需新审批的 current_user 操作；配置目录隔离、只读上限及 restricted 未验收时拒绝 |
| 正常新项目写入 | 自动写入适配未接通，返回 project_file_write_adapter_unavailable；待 M4 |
| 迁移项目文件修改 | 保留原 pending/批准/重试、对象与哈希复核、私有备份行为，各项目队列独立 |
| 非目录列表内的账户文件 | 策略可保存 current_user 范围，实际文件适配待 M4，能力报告明确为 false |
| 项目管理员能力 | 固定不可用，包含从旧 administrator 配置迁移的项目；不请求 UAC |

迁移项目的所有者请求通过 `--project-owner ID --request FILE --manager-data-dir DIR` 路由到本项目队列。FILE 使用旧所有者命令结构，例如 `{"command":"inspect","id":"审批编号"}`、approve、deny、revoke 或 list。MCP 工具没有批准接口。旧 `--approve --data-dir` 不连接管理器托管项目；不要把它与 project-owner 混用。

停止和失败不自动重跑任务。文件审批回执与任务仍主要保存在内存，工作进程结束后失效；日志及私有备份保留。多个普通工作进程和 API 归属检查不构成对任意同账户程序的操作系统沙箱。

## 验证

`scripts/test.ps1` 的 x64 流程依次执行 M1 配置/策略专项、M2 真实普通权限项目专项、M3 回环网关专项和既有核心回归。M2 使用固定原生辅助程序，不采用编码脚本载荷、不启动 legacy administrator 主机；迁移管理员项目仅验证普通工作进程启动及管理员操作拒绝。

报告在 build/validation/x64-Debug-m2.json 与 x64-Release-m2.json，记录程序摘要、检查结果和独立测试 Data 目录。原始失败目录保留，dev.8 回退基线为 build/baseline-dev8-m2-20261010，dev.9 为 build/baseline-dev9-m3-20261010。独立机器、真实客户端和新版本 UAC 验收未执行；M2 当时用户离开，M3 用户已回来并恢复必要人工操作授权，但本阶段不需要提权测试。
