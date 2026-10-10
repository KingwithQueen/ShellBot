# M1 项目配置与统一权限策略

更新日期：2026 年 10 月 10 日。代码版本：`0.1.0-dev.11`。配置格式：`format_version: 1`。

M1 完成配置、统一判断和旧配置显式迁移；M2/M3 接入项目运行与地址。M4 已接通全文件/命令审批、当前账户文件适配和实际 LPAC 受限执行，见 [项目审批与受限执行](PROJECT_APPROVALS.md)。共享管理员代理和远程审批仍待 M5/M6；`--config` 遇到版本化配置会在创建 Data 或初始化执行身份之前拒绝。

## 配置和校验入口

双项目样例在 [projects.example.json](../config/projects.example.json)：full-access 使用当前账户文件范围、never 和普通账户执行；workspace-only 有两个工作目录、目录外拒绝、workspace_auto 和 restricted。受限任务还需要所有者配置 runtime_files 并通过启动实测；样例没有替用户授权任何程序。配置校验不改变已运行实例。

```powershell
.\out\x64\Release\WinShellBot.exe --validate-projects .\config\projects.example.json
```

这个入口不读取环境中的令牌值，不初始化执行身份、不请求 UAC、不创建 Data、不启动监听。新格式目录只做语法和配置冲突检查，不要求目录已存在。兼容项目会调用既有解析器，打开旧工作目录的元数据句柄进行原规则校验，不读取文件正文；旧目录需要存在。状态输出省略真实目录路径及凭据引用，返回项目、策略版本、审批方式和不可用后端的原因。runtime_available:true 表示存在 M2 管理器，runtime_started:false 表示校验没有启动项目；dev.10 的 http_gateway_available:true 表示可以显式开启 M3 网关，校验命令本身没有启动监听。策略的 allow/pending 不能替代后端就绪状态。

| 字段 | 约束与默认值 |
| --- | --- |
| format_version | 必填整数 1；其他格式和无版本注册配置拒绝 |
| projects | 1～16 个项目；project id 区分大小写，由 1～63 个 ASCII 字母、数字、下划线或连字符组成 |
| gateway | 可选；port 默认 8765，仅 1024～65535；public_url 可空，只沿用既有地址语法校验，不进行网络或域名探测 |
| gateway.default_project_id | 只允许引用注册项目；省略表示不绑定旧 `/mcp`；M3 已实现显式默认项目路径 |
| 项目 enabled / policy_revision | 默认 true / 1；revision 必须是非零 uint64，未来由管理器在配置变更时维护 |
| 项目 token_env | 普通新项目必填，仅保存凭据环境变量引用；不同项目不得重用同一引用，Windows 大小写变体也拒绝；工作进程排除全部项目凭据变量；M3 还核验启用项目令牌值有效且不同 |
| 项目 data_dir | 必填绝对磁盘或 UNC 路径；不同项目的 Data 不得相同或互相包含；校验不创建目录 |
| workspaces / extra_directories | 每类最多 16 个目录；两类共用本项目的稳定 id 命名空间，read_only 默认 true |
| default_workspace_id | 必须引用本项目 workspace；省略使用第一个 workspace；无 workspace 不隐式授权 cwd |
| policy | 可省略；默认 always、configured 文件范围、restricted、execution 关闭、管理员关闭，只启用两个元数据工具及四个文件阅读工具 |
| resources | dev.9 可选项目预算；并发、记录、输出、期限、审批和 Job 内存，详见 [管理器](PROJECT_MANAGER.md) |
| approval_ttl_seconds / audit_max_bytes | 默认 300 秒 / 8 MiB；范围沿用 1～600 秒及 4096～67108864 字节；M2 已接入各自工作进程 |

路径拒绝相对路径、父目录跳转、设备路径、数据流、通配符和含糊的尾随点/空格。新项目内目录授权不能在语法上重叠，避免 workspace/额外目录的审批差异及只读差异出现歧义。目录联接、符号链接、大小写和其他别名的**真实对象核验仍由平台运行层完成**；M1 的字符串冲突检查不充当文件沙箱，也不证明 Data 与管理资源的物理隔离。

所有对象拒绝未知字段、重复字段、错误类型和嵌入 NUL。配置总大小最多 64 KiB。`approved`、自报后端已验收、直接令牌字段及模型可调用的改策略字段均不受支持。

## 独立策略字段

policy.approval_mode 为 always、workspace_auto 或 never。always 对全部文件逻辑操作及命令启动/输入返回 pending；workspace_auto 对工作目录内文件操作返回 allow，对已授权额外目录和当前账户其他文件以及命令返回 pending；never 对已获许可的操作返回 allow。列表、搜索、图像与读取采用相同规则。

policy.files.scope 为 configured 或 current_user。configured 仅包含工作目录和显式 extra_directories；current_user 额外允许账户可访问的文件范围，其他位置的 current_user_read_only 默认 true，必须显式 false 才能写。current_user_read_only 只可用于 current_user 范围。显式目录的 read_only 独立保留，不因全文件范围或 never 被覆盖。

policy.execution 保存 enabled、default_identity、administrator_allowed 和可选 runtime_files。普通默认身份只能为 restricted 或 current_user。管理员操作须明确选择身份；正常新配置禁止向 restricted 或 configured 隔离项目授予 administrator_allowed。M5 前项目管理员后端不可用，不能复用 dev.7 单主机代理冒充项目授权。

runtime_files 只允许 restricted，最多 16 个不重复绝对文件路径。运行时限本地盘符、普通单链接文件，分别授予只读/执行，并在整个工作进程期间保持读句柄，阻止写入、删除和替换。列表中的 .exe 才能作为 task_start 程序；可以额外列出它需要的 DLL 或只读资源，不能授予整个系统目录。管理器或任一项目 Data 内的文件不可列为运行资源。ShellBot-SandboxProbe.exe 是旁置的产品启动检查依赖，由主工程自动构建，不是用户测试夹具；缺失时受限后端不可用。

未通过实际后端验证的受限命令返回 restricted_backend_unverified；验证结果只来自可信工作进程，JSON 不能自报。LPAC 启动检查确认实际身份和禁止操作后才开放命令，只读目录仍由操作系统执行。configured 范围的 current_user 命令返回 directory_isolation_unavailable。当前账户范围只读、或任一显式目录只读时，任意账户命令返回 read_only_command_isolation_unavailable。账户任意命令必须明确授予可写账户范围并选择 current_user；该模式属于账户信任，不承诺项目间操作系统隔离。

current_user 文件范围可省略 workspace_id，传入绝对本地盘符 path；平台按物理父目录检查已配置目录的只读和审批类别，不能借绝对路径绕过只读目录。不确定物理类别时直接拒绝；UNC、设备、流和父目录跳转不属于此账户路径适配器。

policy.tools 是现有十三个工具名的显式列表，空数组表示不允许新工具操作；未知及重复工具名拒绝。它独立于目录读写、execution.enabled 和管理员许可，不能单凭列出 task_start 开放命令。

## 统一判断和后续接入契约

[sb_projects.h](../core/include/sb_projects.h) 定义项目、策略、目录和审批绑定结构；[project_policy.c](../core/policy/project_policy.c) 是无副作用判断入口。sb_host 和 sb_request_context 增加可信项目绑定位置；现有单实例传输不从请求填入这些字段；M2 管理器通信为工作进程固定项目和版本。

判断顺序为：可信项目及 actor → 当前策略版本及请求项目断言 → 工具开关 → 已由平台核验的目标授权及读写上限 → 身份与后端能力 → 审批方式。另一个项目的目录对象即使 id 相同也会拒绝；requested_project_id 只能作一致性断言，不能切换 context。目录指针和 current_user_target 必须来自可信的物理路径解析器，不得来自请求中自报的布尔值。

task_status、task_output、task_cancel 在后端确认项目及 actor 的任务归属后返回 allow，无需重复审批；项目停用或工具开关关闭后仍允许取得旧结果和取消本人任务。task_input 单独判断且需要同样的归属核验。M2 的 owned_task 来自本工作进程实际后端的归属查询，不是 MCP 参数；M1 的单元验证不等于真实网关跨项目认证验收。

M4 在 evaluator 外复用原审批队列与所有者入口，绑定项目、actor、policy_revision、实例、规范参数、对象身份/版本、期限和预算；不接受 approved=true。未批准时仅核验路径和对象元数据，执行前重新核验，文件内容变化拒绝继续原许可。分页和重试见 [审批说明](PROJECT_APPROVALS.md)。原有修改摘要、独占打开、备份和审计保留。

## 旧配置显式迁移

未版本化配置仍可按原单实例方式启动，使用 [shellbot.example.json](../config/shellbot.example.json) 的旧字段。不会静默把它变成新项目或更改正在运行的 Data。迁移入口示例：

```powershell
.\out\x64\Release\WinShellBot.exe --migrate-config E:/ShellBotSettings/single.json `
  --output E:/ShellBotSettings/projects.v1.json --project-id default `
  --project-data-dir E:/ShellBotProjects/default/state
```

输出父目录须已存在；输出和 `projects.v1.json.legacy-backup.json` 都必须是新文件。先对一次读取的旧字节校验，再以同一份字节保存备份，输出使用显式 legacy_config 兼容封装；备份及新配置使用当前账户和 SYSTEM 的私有 ACL。不覆盖原配置、已有输出或已有备份，不创建、复制或改写旧/新 Data。新 data_dir 不能与旧显式 Data 或历史默认 Data 在语法上重叠。文件创建不承诺断电事务原子性；已完成备份会在后续输出写入失败时保留。

dev.7 的读取自动、写入本机审批和已启用 user/administrator 命令预授权不能等价映射到一个新的统一审批模式。兼容封装保留原始 roots/read_only、http、execution 档位及 controlled 的 always/rules、审批期限和审计预算；不把原 user/administrator 转成全盘文件工具访问，也不免除文件写入审批。新 project.data_dir 是 M2 工作进程独立数据位置，legacy_config.data_dir 仅保留来源记录；旧任务和审批回执不迁移。兼容项目不允许同时写入新 policy、workspaces 或其他覆盖字段。

兼容 user 的命令仍明确保留原账户信任和预授权语义；controlled 的未完成执行继续拒绝；administrator 不转成普通账户执行，且在新项目层仍因缺少可信代理而拒绝。迁移成功不启动项目，也不触发 UAC。

## 验证和仍待完成项

scripts/build.ps1 另编译开发用 ShellBot-ProjectPolicyTests.exe，不属于产品依赖；ShellBot-SandboxProbe.exe 则为受限后端的运行依赖。x64 验证依次运行 M1、M2、M3、tests/approval_checks.py 的 M4 及旧核心回归。配置专项增加运行资源类型、路径、重复、身份和数量限制；原配置/迁移检查保留。

Debug/Release 的最终结果及程序摘要见 [实现记录](IMPLEMENTATION_STATUS.md) 和 build/validation/x64-Debug-m1.json、x64-Release-m1.json。专项原始目录和失败报告保留在 tests/test-results；dev.7 基线在 build/baseline-dev7-m1-20261010。

M1–M4 已完成本机自动验证；M5 接逐项目管理员授权。历史 HTTP close 问题、独立机器、通用程序兼容性及真实 MCP 客户端验收仍单独记录。
