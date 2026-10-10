# ShellBot

ShellBot 是使用 C17 开发的电脑 MCP 主机。模型服务负责推理，ShellBot 负责本机工具、目录权限、所有者审批和后续任务执行。受控电脑运行程序即可提供工具，不需要安装 ChatGPT 桌面客户端、Node.js 或 Python 运行环境。

Windows 工程 WinShellBot 优先面向 Windows 10/11 x64。通用 C 核心与 Windows 接口分离，便于以后移植 Linux 和 macOS；Windows 7 以后单独编译和验证。

后续开发按已确认的多项目和独立权限方案推进，见 [开发计划](docs/DEVELOPMENT_PLAN.md)及 [项目权限与执行身份](docs/EXECUTION_PERMISSIONS.md)。dev.13 接通 M6 独立所有者审批、客户端 URL 提示、持久请求/任务及生命周期审计，见 [远程审批与恢复](docs/REMOTE_APPROVALS.md)。下一项为 M7 管理界面与候选验收。

## 当前实现

当前版本为 `0.1.0-dev.13`，是开发测试版本。程序只监听本机端口，域名与 cloudflared 隧道由已有环境完成，不要求域名检查；OAuth 与隧道管理暂缓。已实现：

| 能力 | 当前行为 |
| --- | --- |
| MCP 接入 | stdio，以及回环地址上的 Streamable HTTP JSON 响应 |
| host_info / host_capabilities | 系统、版本、运行时长、实际工具、目录标识和限制 |
| fs_list | 授权目录内的文件列表与分页 |
| fs_read | UTF-8、UTF-16 LE/BE 读取、BOM 识别和 Unicode 分页；完整读取返回原始字节 SHA-256 |
| fs_search | 文件名或 UTF-8/UTF-16 内容的字面量搜索，可选递归，内容结果包含行号与有界片段 |
| image_view | PNG/JPEG 原文件作为原生 MCP image 内容返回，并返回头部尺寸信息 |
| fs_write / fs_patch | 默认创建；覆盖和替换保留原始哈希、独占打开与私有备份；UTF-8 原文与结果均限 64 KiB；新项目按独立审批模式执行 |
| 本机审批 | 查看完整请求、批准、拒绝、撤销、过期、绑定参数、并发重试与重启失效 |
| 审批和写入审计 | 私有 JSONL 记录状态、执行结果和备份编号，跨重启保留，日志故障时阻止新批准和写入 |
| 目录配置 | 最多 16 个显式目录，每个目录拥有独立 workspace_id，默认只读 |
| M1 项目配置与策略 | format_version:1 多项目配置校验、三审批模式统一判断、身份与管理员许可分离、旧配置显式备份迁移；以 --manager 显式启动项目 |
| M2 项目管理器 | 本机创建、配置、启停；独立普通工作进程、Data、任务、幂等记录、审批和备份；配额与本人 Job 清理 |
| M3 项目 MCP 网关 | 一个回环端口、/projects/ID/mcp、逐项目 bearer、显式默认项目 /mcp、地址生成、有界并发和进程通信超时 |
| M4 项目审批与受限任务 | 六个文件工具、命令启动和输入统一审批；原队列查看/批准/拒绝/撤销；有界分页和结果重试；LPAC 实际身份与权限验证后开放声明程序 |
| M5 共享管理员代理 | 所有者异步启动 UAC；逐项目可信注册、许可与撤销；显式 identity:administrator 文件与任务；独立审批和归属清理 |
| M6 远程审批与恢复 | 独立所有者凭据与有限页面/API；客户端 URL 提示；本人审批查询/取消；有界私有请求/任务历史；重启中断及完整生命周期审计 |
| 运行保护 | 数据目录实例锁、请求和结果大小上限；HTTP 令牌、Host/Origin 检查 |
| 已有代理接入 | 默认接受本机代理的合法 Host/Origin 且要求 bearer；外部地址配置作为可选限制保留 |
| 后台命令任务 | task_start、task_status、task_output、task_input、task_cancel；显式启用 user/administrator 后提供进程、输出、输入、超时和取消 |
| 执行权限基础层 | 三档配置、真实身份与提升状态查询、受限所有者调用拒绝；管理员代理同账户启动、复用与退出已在 Server 2025 验收；启动 UAC 一次由用户观察确认，跨账户提权未覆盖 |

文件工具检查实际打开的文件或目录句柄对应的最终路径，拒绝越界联接、符号链接、父目录跳转和 NTFS 数据流路径。目录工具路径须为相对路径；新项目只有显式授予 current_user 文件范围后，才可省略 workspace_id 使用绝对本地盘符路径，配置目录只读属性仍生效。文件名搜索跳过重解析目录。没有配置目录时不自动授权当前目录或整台电脑。

本版支持 MCP `2025-03-26`、`2025-06-18`、`2025-11-25` 的初始化与工具子集，并支持 `2026-07-28` 的逐请求元数据、发现、工具调用和请求头校验。声明 URL elicitation 能力的新版客户端可收到 input_required；客户端接受提示仍须实际所有者批准，其他客户端使用 pending/approval_url。HTTP 不建立协议会话，GET `/mcp` 返回 405，未开放 SSE 或标准 MCP tasks 扩展。见 [M6 协议与边界](docs/REMOTE_APPROVALS.md)。

旧单实例 HTTP 使用一个 bearer 身份；项目网关中每个项目使用独立 bearer，路由和凭据共同固定项目。默认不需要填写域名，仍验证 Host/Origin 格式和令牌；可选 public_url 启用域名限制并生成公共地址。实际客户端工具使用与本机自检分别记录。

覆盖与局部修改采用锁定原文件并预先保存私有备份，返回 atomic_save:false，支持冲突拒绝和结果重试，不承诺崩溃原子性，见 [文本修改说明](docs/FILE_EDIT.md)。命令行为见 [命令任务说明](docs/COMMAND_TASKS.md)。M4 受限后端只开放声明的程序与只读运行文件，禁用网络、Win32k 桌面调用和子进程；不保证通用解释器、GUI 或所有构建工具可运行。M6 加载私有历史并标记重启中断，不恢复执行许可或自动重跑。桌面控制、PDF/Office/OCR 和项目管理界面仍待实现。

未版本化的单实例运行配置仍使用 dev.7 的 controlled、user、administrator；原 controlled 命令保持不可用，原 user/administrator 预授权行为保留。项目配置将审批、文件范围、身份与管理员许可分开，通过 --manager 启动，加 --http 开启项目网关；--config 保持旧入口。LPAC 和共享管理员仅用于正常新格式项目，管理员许可不兼容受限/目录隔离项目；旧管理员迁移不会自动接入共享代理。见 [项目配置说明](docs/PROJECT_CONFIGURATION.md)及 [项目权限与执行身份](docs/EXECUTION_PERMISSIONS.md)。

## 构建与验证

使用 Visual Studio 的 C++ 桌面开发工具及 Windows SDK。现有工程已设置 `.c`、`/std:c17`、`/utf-8`，自有代码开启四级警告并将警告视为错误；Release 使用静态运行库。当前开发机使用 VS 2026 v145 和 Windows SDK 10.0.26100.0。[微软 C17 工具链要求](https://learn.microsoft.com/en-us/cpp/overview/install-c17-support?view=msvc-170)

在仓库目录运行：

```powershell
.\scripts\build.ps1 -Configuration Debug
.\scripts\build.ps1 -Configuration Release
python .\tests\smoke.py --exe .\out\x64\Release\WinShellBot.exe
# 或者保存包含可执行文件校验值的验证报告：
.\scripts\test.ps1 -Configuration Release
# 分别验证核心功能，明确排除仍在调查的关闭连接诊断：
.\scripts\test.ps1 -Configuration Release -SkipCloseProbe
# 核心报告必须匹配当前 Release 程序才能生成测试包：
# 当前打包器另要求旧 dev.7 原生专项凭据；dev.13 M6 本轮不生成新包。
.\scripts\package-test.ps1
```

也可以在 Visual Studio 中打开 `ShellBot.slnx` 构建。产物位于 `out/x64/Debug/WinShellBot.exe` 或 `out/x64/Release/WinShellBot.exe`。Python 仅用于开发验证，不属于发行程序的运行依赖。

验证程序启动实际进程和 HTTP 监听，覆盖多目录、中文路径、Unicode 分页、图像字节、目录联接逃逸、审批、禁止覆盖、畸形请求、令牌、并发和停止。dev.7 已在 Windows 11 build 26300 验证普通及管理员任务；改用 C 测试辅助程序后，用户确认没有新杀毒告警，UAC 仅启动代理时一次。测试包包含辅助程序及源码，产品不依赖它。当前机器上的构建与调用结果见[实现与验证记录](docs/IMPLEMENTATION_STATUS.md)。开发机主动关闭 HTTP 连接时仍有连接重置，完整套件因此未通过；核心套件与该诊断分别记录，不隐藏失败。

2026 年 10 月 6 日，Windows Server 2025 上 dev.2 的原脚本复测核心通过、关闭连接 0/100 失败；新增 HTTP-only 诊断整体通过，关闭连接和 fresh keep-alive 对照各 0/100 失败。此前同机出现过 2/100 关闭失败，本次通过不代表根因已修复。dev.2 的反馈来自控制台，该版本的完整 report.json 尚未回传。开发机仍可复现连接重置，Win10/11、公网与真实模型客户端保留待验收，见[实现与验证记录](docs/IMPLEMENTATION_STATUS.md)和 [Server 2025 测试步骤](docs/SERVER_2025_TEST.md)。

随后已读取 dev.3 的两份完整报告：Server 2025 Datacenter build 26100，程序 SHA-256 与交付包一致，两轮各通过 152 项核心和基本 HTTP 检查，新增审计 ACL、内容脱敏和重启保留均通过。close 合计 1/200 失败，唯一失败为响应头收到 0 字节时发生 10054；fresh keep-alive 为 0/200 失败。dev.3 的上述本机功能已获得独立机器证据，偶发连接重置与实际客户端调用仍待验证；公网 OAuth 已按最新决定暂缓。

dev.4 的 Server 2025 完整报告已核验：160 项核心和基本 HTTP 检查通过，关闭连接与 fresh keep-alive 各 0/100 失败，程序 SHA-256 匹配交付包。新增内容搜索、外部域名配置和请求头校验已通过独立机器基础验证；这一轮通过不证明偶发重置已修复，也不包含真实隧道调用。已有域名可使用 [独立端点检查脚本](tests/Test-ShellBot-Endpoint.ps1)，说明见 [外部访问配置](docs/EXTERNAL_ACCESS.md)。

2026 年 10 月 7 日，dev.5 在 Server 2025 Datacenter build 26100 上的 195 项核心与基本 HTTP 检查全部通过，程序 SHA-256 匹配交付包。覆盖、局部替换、完整摘要、私有原文备份及备份 ACL，以及无需填写域名的代理请求头检查获得独立机器证据。close 为 1/100 失败、fresh keep-alive 为 0/100，整体 needs_investigation；连接问题仍待修复，命令任务和实际客户端工具使用仍待完成。

## 本机试用

支持启动本机 MCP 子进程的客户端可以使用以下命令和参数；目录必须预先存在：

```text
command: E:\Program\ShellBot\out\x64\Release\WinShellBot.exe
args: --stdio --root C:\ShellBotWorkspace
```

`--root` 为目录设置标识 `workspace`，始终只读。多个目录请复制并修改 [config/shellbot.example.json](config/shellbot.example.json)，然后使用 `--config 配置文件路径`。配置目录必须使用绝对路径；`--root` 和 `--config` 不能同时使用。root 的 `read_only` 默认 `true`；设为 `false` 只允许提出创建请求，仍需本机所有者批准具体目标和内容。

M1 新格式可用 `WinShellBot.exe --validate-projects 配置文件` 校验，样例为 [config/projects.example.json](config/projects.example.json)。`--migrate-config` 会生成新配置和原始字节备份，不修改旧 Data；具体参数及当前不可用的组合见 [项目配置说明](docs/PROJECT_CONFIGURATION.md)。新格式尚不作为上述 --config 的运行输入。

项目 MCP 使用 `--manager --http --projects 注册配置 --manager-data-dir 独立目录` 启动，再通过 `--project-start ID` 显式启用项目。每个启用项目必须在其 token_env 中提供不同的有效令牌；地址为 `http://127.0.0.1:8765/projects/ID/mcp`，`--project-links ID` 返回本机和可选公共地址。旧 `/mcp` 仅在 gateway.default_project_id 显式绑定时提供。完整步骤与限制见 [项目网关](docs/PROJECT_GATEWAY.md)。

HTTP 本机开发示例：

```powershell
$env:SHELLBOT_TOKEN = & .\out\x64\Release\WinShellBot.exe --generate-token
.\out\x64\Release\WinShellBot.exe --http --root C:\ShellBotWorkspace --port 8765
```

入口为 `http://127.0.0.1:8765/mcp`。客户端使用 `Authorization: Bearer <令牌>`、`Content-Type: application/json`、`Accept: application/json, text/event-stream`；初始化后的请求使用协商的 `MCP-Protocol-Version`。每次 POST 一个 JSON-RPC 消息；通知返回 202，工具请求返回 JSON。`/healthz` 也需要令牌。HTTP 默认保持连接，空闲连接有超时；Ctrl+C 或 Ctrl+Break 停止监听。当前只接收有 Content-Length 的请求，不接收 chunked 请求。

令牌从环境读取，不接受 URL 令牌，不打印凭据。默认运行目录为当前用户的 `%LOCALAPPDATA%/ShellBot`，可用 `--data-dir 绝对路径` 指定独立目录。同一数据目录只能由一个实例占用。诊断走 stderr；stdio 模式 stdout 只输出协议消息。

如果环境已经提供隧道，在配置的 http.public_url 中填写 bot.example.com，或启动时加上 --public-url bot.example.com。裸域名默认 HTTPS，客户端地址为 https://bot.example.com/mcp；本机仍监听 127.0.0.1，现有 bearer 与本机审批继续使用。程序不会启动或管理隧道，详见[已有隧道与域名配置](docs/EXTERNAL_ACCESS.md)。

2026 年版本的请求不走 initialize；每次在 `params._meta` 中提供 `io.modelcontextprotocol/protocolVersion: "2026-07-28"` 与对象类型的 `io.modelcontextprotocol/clientCapabilities`。HTTP 还须提供一致的 `MCP-Protocol-Version`、`Mcp-Method`，调用工具时提供 `Mcp-Name`。stdio 同一连接不能混用新旧协议方式。

审批通过同账户的受限本地命名管道管理，MCP 不提供批准工具。使用 `--list-approvals`、`--inspect ID`、`--approve ID`、`--deny ID`、`--revoke ID`，并指定与主机相同的 `--data-dir`；`--stop` 停止 HTTP 主机。`fs_write` 提交 `workspace_id/path/text` 后返回待审批 ID；查看、批准后，以相同参数加上 `approval_id` 重试。参数摘要包含规范目标、父目录的卷及 128 位文件标识、内容 SHA-256 和字节数；批准只被原子消费一次。

旧单实例入口默认审批期限 300 秒，可配置 `approval_ttl_seconds: 1..600`，最多 64 条内存记录，重启许可失效；正常项目另保留 M6 私有历史和任务键。创建操作通过已验证父目录句柄写入并刷新临时文件，再禁止覆盖地提交；异常中断的临时文件不自动按名清理。同账户程序属于管理信任范围，user/administrator 为预先授予的账户级信任模式。

审批与创建事件保存为数据目录下的 `audit-实例编号.jsonl`，当前账户和 SYSTEM 可以访问。正文及令牌不写入日志，每条事件刷新落盘。配置 `audit_max_bytes` 默认每实例 8 MiB，故障或达到预算后阻止新批准及写入；如果文件已提交而结果记录失败，保留完成状态并返回 audit_warning，重试不再次执行。旧日志保留，旧审批仍不恢复，没有崩溃任务恢复，详见[审计行为与限制](docs/AUDIT.md)。

文本默认每次最多读取 64 KiB；后续页面使用结果的 `next_offset` 和 `encoding`。不确定编码和二进制内容返回错误，不默默替换字符。图像限制为 2 MiB 和头部标记的 1600 万像素；本版仅做格式头检查，不完整解码、缩放或转换图像。搜索最多检查 1000 个条目、递归 8 层，在操作之间检查 2 秒预算；网络磁盘等单次系统调用仍可能阻塞，时间检查不等于硬超时。

fs_search 的 mode 默认 filename；设为 content 可以搜索 UTF-8/UTF-16 文本，返回行号、UTF-8 字节列号和最多 1024 字节的命中片段。默认读取每文件前 256 KiB、总计 4 MiB；查询不完整时明确返回 truncated 和扫描计数，二进制/无效编码被检测到后跳过，不遍历重解析目录。详见[文本内容搜索](docs/CONTENT_SEARCH.md)。

## 后续架构

已确认采用普通权限管理器、每项目独立工作进程和 /projects/{project_id}/mcp 链接；各项目独立凭据、多个工作目录、任务与审计。审批提供 always、workspace_auto、never 三种模式，目录外访问与管理员使用许可独立设置。共享管理员代理只服务已获所有者授权的项目，一个项目获准不使其他项目拥有管理员权限；cwd 不是命令沙箱，受限执行通过验证前保持不可用。

当前假定外部访问环境已经准备好，程序监听本地端口，域名由外部代理完成；public_url 是可选限制和地址报告功能。现有 bearer、目录权限与所有者审批继续使用，不要求域名检查。桌面控制、文档后端及其他平台按后续阶段推进。

| 位置 | 内容 |
| --- | --- |
| core/ | 通用配置、协议、工具和 Unicode 处理，无 Windows 头文件 |
| WinShellBot/app/ | Windows 命令行、stdio/HTTP 接入及程序生命周期 |
| WinShellBot/platform/ | Windows 路径、文件、系统信息、随机数及实例锁 |
| third_party/ | 固定原生组件版本、来源校验和许可 |
| config/ / scripts/ / tests/ | 配置样例、构建脚本和实际调用验证 |
| docs/ARCHITECTURE.md | 总体架构与最终功能方向 |
| docs/PHASE_1_PLAN.md | 第一阶段既有里程碑和剩余验收 |
| docs/DEVELOPMENT_PLAN.md | 当前多项目与权限迭代顺序、交付条件 |
| docs/EXECUTION_PERMISSIONS.md | 文件范围、审批、执行身份与逐项目管理员授权 |

运行数据、凭据、Visual Studio 缓存、测试和构建产物不纳入版本控制。开发和验证使用独立目录，不迁移其他项目的运行数据。

许可采用 [MIT 模板](LICENSE.txt)，版权年份和主体需在首次正式发行前补全；第三方组件保留各自许可，见[依赖记录](third_party/README.md)。

旧版本包、基线与原始报告继续保留。M3 已由用户提交推送为 `cb2d26b`；本轮 M4 是 main 上未提交、未推送的本地更改，没有新包。M3 完整回退基线为 build/baseline-dev10-m4-20261010。当前验收范围与剩余项目以 IMPLEMENTATION_STATUS.md 和 [M4 交接](docs/HANDOFF_20261010_M4.md)为准，旧版本结果只适用于对应程序摘要。
