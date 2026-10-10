# M3 项目 MCP 网关

更新日期：2026 年 10 月 10 日。版本：`0.1.0-dev.10`。普通管理器可在一个回环端口提供多个独立项目地址；本机项目生命周期及权限边界见 [项目管理器](PROJECT_MANAGER.md)。

## 启动与连接地址

配置仍使用 `format_version:1`。每个启用项目指定独立 `token_env`；环境变量值必须为 32～256 字节、不含空格的可见 ASCII，启用项目之间不能使用相同值。缺失、无效或重复凭据会阻止整个网关启动，不留下部分可用的监听。停用项目无需提供凭据。

```powershell
$bot = 'E:/Program/ShellBot/out/x64/Release/WinShellBot.exe'
$env:SHELLBOT_FULL_TOKEN = & $bot --generate-token
$env:SHELLBOT_WORKSPACE_TOKEN = & $bot --generate-token
& $bot --manager --http --projects E:/ShellBotSettings/projects.json `
  --manager-data-dir E:/ShellBotProjects/manager/state
```

环境变量名须与实际配置匹配，目录须按项目配置预先准备好。管理器输出 `manager_ready:true,http_gateway:true` 后持续运行。没有 `--http` 时仍只提供 M2 本机管理通道，不读取令牌值或监听端口。

另一个同账户、普通权限终端执行：

```powershell
& $bot --project-start full-access --manager-data-dir E:/ShellBotProjects/manager/state
& $bot --project-start workspace-only --manager-data-dir E:/ShellBotProjects/manager/state
& $bot --project-links full-access --manager-data-dir E:/ShellBotProjects/manager/state
& $bot --project-status --manager-data-dir E:/ShellBotProjects/manager/state
```

`--project-links` 返回 `local_mcp_url`、可选 `public_mcp_url`、`token_env` 引用、`gateway_running`、`enabled` 及 `default_route`，不返回令牌值。停止的项目也可生成地址，但收到 MCP 请求时不会自动启动。工具列表、主机能力、工作目录和任务始终来自该地址对应的工作进程。

| 配置 | 地址与行为 |
| --- | --- |
| gateway.port 省略 | 监听 `127.0.0.1:8765` |
| 项目 full-access | `http://127.0.0.1:8765/projects/full-access/mcp` |
| gateway.public_url 为 `https://bot.example.com` | 另生成 `https://bot.example.com/projects/full-access/mcp`，约束公共 Host/Origin |
| public_url 省略或为空 | 只生成本机地址；仍允许已有代理保留语法合法的 Host/Origin，无域名探测 |
| default_project_id 省略 | `/mcp` 与 `/healthz` 返回 404 |
| default_project_id 为 full-access | `/mcp`、`/healthz` 只绑定 full-access，仍只接受该项目令牌 |
| 项目健康状态 | `GET /projects/full-access/healthz`；需要该项目令牌且工作进程正在运行 |

项目 id 区分大小写。只接受规范地址，不接受百分号转义、路径跳转、重复斜杠、反斜杠、尾随斜杠或查询参数。令牌不能放在 URL 中。路由和令牌共同确定项目；不存在依据 `project_id`、`actor`、`clientInfo` 或转发头切换项目的入口。

现有隧道只需转发同一个回环端口；ShellBot 不安装、修改或监管隧道，也不探测 DNS、用户域名或公网连通性。多个地址在本机网关的验证不替代实际隧道及客户端验收。

## 协议与身份

客户端发送 `Authorization: Bearer <该项目令牌>`、`Content-Type: application/json` 和包含 `application/json` 的 Accept。支持已有三个 2025 初始化协议，以及 `2026-07-28` 的逐请求协议；后者每次提供对应的 `params._meta`、`MCP-Protocol-Version`、`Mcp-Method`，工具调用还须提供匹配的 `Mcp-Name`。原有 base64 请求头编码及重复字段检查保留。

管理器只传实际 HTTP 协议头和请求给已经绑定的工作进程；工作进程以无会话 HTTP 方式校验，不能借本机 stdio 状态绕过请求头匹配。通知保留空响应 HTTP 202，协议拒绝保留原 HTTP 状态，文件和图像工具保留原 MCP 内容结构。GET MCP 地址不提供 SSE，返回 405。

项目 bearer 调用者固定为 `project-mcp`；本机管理请求固定为 `manager-owner`。这两个调用者的任务及幂等键独立，不能取得对方任务结果、输入或取消。一个项目目前只有一个 bearer 身份；多个客户端共用该项目令牌时共享其调用者身份。凭据和注册快照在管理器启动时固定，更换令牌或默认项目需要明确停止并重新启动管理器；旧任务和审批不恢复。

管理命令和所有者批准只通过原受保护本机通道。MCP 没有启动/停止项目、修改策略、批准或注册项目接口。迁移项目的 HTTP 文件修改仍需 `--project-owner` 批准，批准后由原调用者携带相同参数与 approval_id 重试；正常新项目完整审批及写入适配仍待 M4。

## 并发、预算与错误

网关固定四个 HTTP 服务线程、四个应用层等待连接位置，所有者通道另有四个固定线程。每个工作进程一次处理一个请求：认证通过后先预留该项目，再读取请求体；同项目忙时返回 429，不在项目侧排队。请求完成、格式拒绝或通信失败均释放预留。慢请求或一个工作进程失败时，其他项目可由剩余线程处理；全局服务线程全部占用时仍会受共享预算限制。

HTTP 请求体最多 1 MiB，要求 Content-Length，不支持 chunked；私有工作进程请求帧另有 64 KiB 协议封装预算，响应帧最多 4 MiB。读取正文有 10 秒循环期限，底层单次网络等待最多 5 秒；这不是整个 HTTP 请求的精确 10 秒上限。工作进程通信共用独立 8 秒期限。TCP 连接保持与协议会话无关，空闲保持最多 2 秒；线程和排队数量不随请求增加。

| 状态 | 处理方式 |
| --- | --- |
| 401 | 缺失、错误或属于另一项目的令牌；返回 Bearer challenge |
| 404 | 未注册或非规范的已识别路径、不绑定默认项目的 `/mcp`；协议未知方法也可能为 404 |
| 400 | 地址转义/跳转/查询、重复安全头、畸形 JSON、请求头与协议正文不匹配 |
| 403 | Host/Origin 不满足约束，或可信项目绑定校验失败 |
| 405 | MCP 地址的 GET/DELETE 等不支持方法，或健康地址的非 GET 方法 |
| 413 | 正文或私有请求封装超过预算 |
| 429 | 项目已有请求；没有在项目侧排队，可等待后重试查询 |
| 503 | 项目停用、停止、管理器退出，或工作进程断线/通信超时 |

权限策略拒绝还可表现为 HTTP 200 中的 MCP `isError:true`，不能仅凭 HTTP 成功判定操作成功。通信超时会终止本管理器拥有的该项目 Job 并标记失败；有副作用操作可能已经执行，错误明确要求先复核，不自动重启或重发。恢复须由所有者显式 `--project-start`；实例编号改变后旧任务请求拒绝。

M4 前不开放未完成的受限执行、目录外文件适配及完整审批；M5 前项目管理员后端仍不可用。普通账户任意命令具有账户级信任权限，多进程及 API 校验不构成操作系统沙箱。

## 验证记录

`tests/gateway_checks.py` 启动真实回环网关、独立工作进程及固定原生任务辅助程序。覆盖凭据启动拒绝、跨项目认证、规范路由、默认项目、无域名配置、连接生成、协议头/通知/旧初始化、同名工作目录、工具开关、慢正文并发、任务和输入幂等、所有者与 bearer 归属、旧修改审批、进程通信超时及显式重启。测试凭据只在环境与内存中生成；保留产物逐文件检查不含令牌值。

Debug/Release 最终结果及摘要见 [实现记录](IMPLEMENTATION_STATUS.md)，报告为 build/validation/x64-{Debug,Release}-m3.json。M2 回退基线保存在 build/baseline-dev9-m3-20261010。历史 HTTP Connection: close 重置仍待调查；独立机器、实际 MCP 客户端和用户已有公网隧道尚未验收。用户本轮已恢复必要人工测试授权，但 M3 本机验证不需要 UAC，未启动管理员专项。
