# M1 项目配置与统一权限策略

更新日期：2026 年 10 月 10 日。代码版本：`0.1.0-dev.8`。配置格式：`format_version: 1`。

M1 完成配置数据模型、严格校验、统一 allow/deny/pending 判断和旧配置显式迁移。项目工作进程、网关认证、完整审批适配、受限执行、共享管理员代理和远程审批仍按 M2–M6 实现。新注册配置现在只能校验，不能用于启动项目；`--config` 遇到版本化配置会在创建 Data 或初始化执行身份之前拒绝。

## 配置和校验入口

双项目样例在 [projects.example.json](../config/projects.example.json)：full-access 使用当前账户文件范围、never 和普通账户执行；workspace-only 有两个工作目录、目录外拒绝、workspace_auto 和 restricted。后者即使 execution.enabled 为 true，受限命令也保持不可用。项目样例只用于配置校验，不改变已运行实例。

```powershell
.\out\x64\Release\WinShellBot.exe --validate-projects .\config\projects.example.json
```

这个入口不读取环境中的令牌值，不初始化执行身份、不请求 UAC、不创建 Data、不启动监听。新格式目录只做语法和配置冲突检查，不要求目录已存在。兼容项目会调用既有解析器，打开旧工作目录的元数据句柄进行原规则校验，不读取文件正文；旧目录需要存在。状态输出省略真实目录路径及凭据引用，返回项目、策略版本、审批方式和不可用后端的原因。`runtime_available:false` 是固定的当前能力声明；策略的 allow/pending 不是项目已经可运行的证明。

| 字段 | 约束与默认值 |
| --- | --- |
| format_version | 必填整数 1；其他格式和无版本注册配置拒绝 |
| projects | 1～16 个项目；project id 区分大小写，由 1～63 个 ASCII 字母、数字、下划线或连字符组成 |
| gateway | 可选；port 默认 8765，仅 1024～65535；public_url 可空，只沿用既有地址语法校验，不进行网络或域名探测 |
| gateway.default_project_id | 只允许引用注册项目；省略表示不绑定旧 `/mcp`，实际路由留待 M3 |
| 项目 enabled / policy_revision | 默认 true / 1；revision 必须是非零 uint64，未来由管理器在配置变更时维护 |
| 项目 token_env | 普通新项目必填，仅保存凭据的环境变量引用；不同项目不得重用同一引用，Windows 大小写变体也拒绝；实际令牌隔离由 M2/M3 建立 |
| 项目 data_dir | 必填绝对磁盘或 UNC 路径；不同项目的 Data 不得相同或互相包含；校验不创建目录 |
| workspaces / extra_directories | 每类最多 16 个目录；两类共用本项目的稳定 id 命名空间，read_only 默认 true |
| default_workspace_id | 必须引用本项目 workspace；省略使用第一个 workspace；无 workspace 不隐式授权 cwd |
| policy | 可省略；默认 always、configured 文件范围、restricted、execution 关闭、管理员关闭，只启用两个元数据工具及四个文件阅读工具 |
| approval_ttl_seconds / audit_max_bytes | 默认 300 秒 / 8 MiB；范围沿用 1～600 秒及 4096～67108864 字节；M1 仅保存，项目配额接入留待 M2 |

路径拒绝相对路径、父目录跳转、设备路径、数据流、通配符和含糊的尾随点/空格。新项目内目录授权不能在语法上重叠，避免 workspace/额外目录的审批差异及只读差异出现歧义。目录联接、符号链接、大小写和其他别名的**真实对象核验仍由平台运行层完成**；M1 的字符串冲突检查不充当文件沙箱，也不证明 Data 与管理资源的物理隔离。

所有对象拒绝未知字段、重复字段、错误类型和嵌入 NUL。配置总大小最多 64 KiB。`approved`、自报后端已验收、直接令牌字段及模型可调用的改策略字段均不受支持。

## 独立策略字段

policy.approval_mode 为 always、workspace_auto 或 never。always 对全部文件逻辑操作及命令启动/输入返回 pending；workspace_auto 对工作目录内文件操作返回 allow，对已授权额外目录和当前账户其他文件以及命令返回 pending；never 对已获许可的操作返回 allow。列表、搜索、图像与读取采用相同规则。

policy.files.scope 为 configured 或 current_user。configured 仅包含工作目录和显式 extra_directories；current_user 额外允许账户可访问的文件范围，其他位置的 current_user_read_only 默认 true，必须显式 false 才能写。current_user_read_only 只可用于 current_user 范围。显式目录的 read_only 独立保留，不因全文件范围或 never 被覆盖。

policy.execution 保存 enabled、default_identity 和 administrator_allowed。普通默认身份只能为 restricted 或 current_user。管理员操作须明确选择身份；正常新配置禁止向 restricted 或 configured 隔离项目授予 administrator_allowed。管理员配置许可与代理运行状态分开，M1 的项目管理员后端固定不可用，不能复用 dev.7 单主机代理冒充项目授权。

受限命令返回 restricted_backend_unverified。configured 范围的 current_user 命令返回 directory_isolation_unavailable。对于正常新项目，当前账户文件范围只读、或任一显式目录只读时，任意账户命令返回 read_only_command_isolation_unavailable；普通进程无法强制执行这些写入上限。使用账户任意命令必须明确授予可写的账户范围并选择 current_user；这仍属于账户信任模式，不承诺项目间操作系统隔离。

policy.tools 是现有十三个工具名的显式列表，空数组表示不允许新工具操作；未知及重复工具名拒绝。它独立于目录读写、execution.enabled 和管理员许可，不能单凭列出 task_start 开放命令。

## 统一判断和后续接入契约

[sb_projects.h](../core/include/sb_projects.h) 定义项目、策略、目录和审批绑定结构；[project_policy.c](../core/policy/project_policy.c) 是无副作用判断入口。sb_host 和 sb_request_context 增加可信项目绑定位置；现有单实例传输不从请求填入这些字段，新项目传输尚未接通。

判断顺序为：可信项目及 actor → 当前策略版本及请求项目断言 → 工具开关 → 已由平台核验的目标授权及读写上限 → 身份与后端能力 → 审批方式。另一个项目的目录对象即使 id 相同也会拒绝；requested_project_id 只能作一致性断言，不能切换 context。目录指针和 current_user_target 必须来自可信的物理路径解析器，不得来自请求中自报的布尔值。

task_status、task_output、task_cancel 在后端确认项目及 actor 的任务归属后返回 allow，无需重复审批；项目停用或工具开关关闭后仍允许取得旧结果和取消本人任务。task_input 单独判断且需要同样的归属核验。owned_task 是供未来后端传递的可信结果，不是 MCP 参数；M1 的单元验证不等于真实网关跨项目认证验收。

审批绑定契约包含项目、actor、policy_revision、操作摘要、对象身份、版本、期限和字节预算。当前 evaluator 没有接受 approved=true 或消费批准记录的接口：pending 仍需 M4 的审批状态机适配。M4 必须在读取正文前判断，并在执行前重新核验；分页许可、版本变化、预算消费和策略收窄的运行验证留待该里程碑。原有文件修改的摘要、锁、备份及审计保护继续保留。

## 旧配置显式迁移

未版本化配置仍可按原单实例方式启动，使用 [shellbot.example.json](../config/shellbot.example.json) 的旧字段。不会静默把它变成新项目或更改正在运行的 Data。迁移入口示例：

```powershell
.\out\x64\Release\WinShellBot.exe --migrate-config E:/ShellBotSettings/single.json `
  --output E:/ShellBotSettings/projects.v1.json --project-id default `
  --project-data-dir E:/ShellBotProjects/default/state
```

输出父目录须已存在；输出和 `projects.v1.json.legacy-backup.json` 都必须是新文件。先对一次读取的旧字节校验，再以同一份字节保存备份，输出使用显式 legacy_config 兼容封装；备份及新配置使用当前账户和 SYSTEM 的私有 ACL。不覆盖原配置、已有输出或已有备份，不创建、复制或改写旧/新 Data。新 data_dir 不能与旧显式 Data 或历史默认 Data 在语法上重叠。文件创建不承诺断电事务原子性；已完成备份会在后续输出写入失败时保留。

dev.7 的读取自动、写入本机审批和已启用 user/administrator 命令预授权不能等价映射到一个新的统一审批模式。兼容封装保留原始 roots/read_only、http、execution 档位及 controlled 的 always/rules、审批期限和审计预算；不把原 user/administrator 转成全盘文件工具访问，也不免除文件写入审批。新 project.data_dir 是未来独立数据位置，legacy_config.data_dir 仅保留来源记录；旧任务和审批回执不迁移。兼容项目不允许同时写入新 policy、workspaces 或其他覆盖字段。

兼容 user 的命令仍明确保留原账户信任和预授权语义；controlled 的未完成执行继续拒绝；administrator 不转成普通账户执行，且在新项目层仍因缺少可信代理而拒绝。迁移成功不启动项目，也不触发 UAC。

## 验证和仍待完成项

scripts/build.ps1 另编译开发用 ShellBot-ProjectPolicyTests.exe，不属于产品运行依赖。scripts/test.ps1 的 x64 分支先运行 tests/project_checks.py，再运行旧工具核心回归。专项覆盖三模式、跨项目同名目录、只读和目录外上限、任务归属、策略版本、不可用后端、严格配置拒绝、六类旧执行配置迁移、实际解析后的兼容策略、字节备份、重复输出拒绝和旧 Data 保留。

Debug/Release 的最终结果及程序摘要见 [实现记录](IMPLEMENTATION_STATUS.md) 和 build/validation/x64-Debug-m1.json、x64-Release-m1.json。专项原始目录和失败报告保留在 tests/test-results；dev.7 基线在 build/baseline-dev7-m1-20261010。

M2 继续普通权限管理器、每项目工作进程、独立数据及凭据/任务/审计、配额和故障处理；M3 再接项目路由与认证；M4/M5 接实际审批、受限执行和逐项目管理员授权。历史 HTTP close 问题、独立机器及真实 MCP 客户端验收仍单独记录。
