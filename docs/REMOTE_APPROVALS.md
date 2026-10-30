# M6 远程所有者审批与持久记录

版本 `0.1.0-dev.13`。正常 `format_version:1` 项目提供此功能，旧兼容入口保留原行为。开发机 HTTP、浏览器和进程验证与另一设备、真实 MCP 客户端验收分别记录。

## 启用

注册配置的 `gateway` 加入 `owner_token_env`，例如：

```json
{"port":8765,"owner_token_env":"SHELLBOT_OWNER_TOKEN"}
```

启动前设置独立环境变量值，再运行 `--manager --http`。值须为 32～256 字节可见 ASCII，不得与任何项目令牌相同；变量名也不能与项目变量重名，忽略大小写。无效值阻止网关启动。省略字段时所有者路由返回 404；仅本机管理器模式不读取远程凭据。

打开 `http://127.0.0.1:8765/owner`，输入所有者凭据，选择项目及普通/管理员身份，核对完整规范参数、调用方、摘要、实例、策略版本和期限后点击决定。打开页面、选择记录及客户端接受提示均不会批准。凭据只在页面内存中保存，连接后输入框清空，清除按钮销毁凭据及选择状态；不放入 URL、Cookie、本地存储、MCP 或审计。

已有 HTTPS 代理可转发同一个端口。可选 `gateway.public_url` 生成公共审批链接并约束 Host/Origin；省略时生成本机地址，另一设备须在已有公共地址打开页面。ShellBot 不增加 TLS、OAuth、域名探测或隧道管理。远程接口不会启动项目、管理员代理或 UAC。

## 有限所有者接口

`GET /owner`、`GET /owner.js`、`GET /owner.css` 提供公开页面壳。`POST /owner/api` 在读取正文和查找项目之前验证独立 `Authorization: Bearer …`。项目令牌不能审批，所有者令牌也不能作为项目令牌调用 MCP。页面使用同源请求，无 CORS；拒绝重复安全头、查询参数、非规范路径和分块请求，正文最多 64 KiB。页面设定 CSP、禁止嵌入、no-referrer 和 no-store。

| command | 字段及行为 |
| --- | --- |
| projects | 仅此字段；返回正常启用项目及状态，不返回 Data 或凭据 |
| list | project_id、backend；当前审批元数据 |
| inspect | 同上加 id；当前请求完整规范参数 |
| approve / deny / revoke | 同上加 id、expected_sha256、expected_host_instance_id、expected_policy_revision；审批锁内核对显示过的绑定 |
| history | project_id、backend，可选 id、offset、limit；分页元数据，指定 id 时返回完整私有历史 |

backend 只能为 ordinary 或 administrator，两审批池独立，不能跨项目或身份消费批准。接口不接受 actor、任意 MCP 请求、配置修改、项目启停或代理许可。管理员代理须已由本机所有者启动，并保持项目许可。凭据不传给代理；管理器使用既有私有通道，决策审计 actor 固定为 remote-owner。停止项目须本机显式启动后查询历史。

文件范围、只读、工具启用、参数与物理目标版本、调用方、实例和期限检查继续生效。允许的普通账户/管理员程序仍保留其系统权限；独立凭据建立协议边界，不宣称能抵抗同账户任意程序或管理员。

## MCP 交互

正常项目增加 approval_status、approval_cancel，只查询或取消本调用方请求；管理员请求保留 identity:administrator。这两个工具不能批准。重启中断请求可查历史元数据，不恢复许可。

2026-07-28 请求明确声明 `_meta["io.modelcontextprotocol/clientCapabilities"].elicitation.url:{}` 时才返回 resultType:input_required。inputRequests 使用 elicitation/create、mode:url 和无凭据链接，requestState 为不授予权限的请求编号。客户端以相同原工具参数、requestState、对应 inputResponses/action 继续；accept 仍须实际所有者批准，decline/cancel 只取消本人请求。接受后批准未完成时返回 pending，不重复弹出同一提示。

未声明 URL、仅 form、2025 协议或旧配置使用原 pending 结构；已启用入口时正常项目另返回 approval_url。HTTP 不开放 SSE、2025 服务器反向请求或标准 MCP tasks 扩展。依据：[MCP 2026 URL elicitation](https://modelcontextprotocol.io/specification/2026-07-28/client/elicitation)。

## 记录与恢复

普通状态在项目 Data 的 state-持久实例.jsonl；管理员状态固定在 Data/administrator-state，不随普通 worker 更换目录。每次开启 backend 状态库产生独立 state_instance_id，区别于普通 host_instance_id；代理单独重启也形成新持久实例。旧 dev.12 administrator-实例 目录保留，不自动迁移批准。独立审计不写正文、stdin 或输出；私有 state 保存参数、有限回执、任务结果、输入摘要/EOF/投递状态及有限输出。

任务在 ResumeThread 前刷新启动意图，在 WriteFile 前刷新输入意图；记录启动、自然完成、取消请求及结果、期限、输出限额和输入投递。队列成功、WriteFile 成功与程序实际消费是不同信号。stdin 正文不保存，不跨进程重投。每个输出流历史最多 256 KiB，保留实际字节数及 history_truncated 标志；运行期输出预算不变。所有者指定任务 id 查 history 可读保留的 base64 输出；task_status/output 当前执行接口仍绑定活动实例，不控制历史 PID。

重启仅加载历史，不加载执行许可、活动任务或幂等执行。旧 pending/approved/running/starting 显示 interrupted、previous_status、active_instance:false、expires_in_ms:0，恢复标记 automatic_replay:false。已启动任务的 request_key 在历史保留期间禁止跨 backend 实例复用；包括普通主机编号不变的代理独立重启。当前实例原键重试仍返回原任务；重启后先复核并显式用新键。已刷新 completed/failed/cancelled/timed_out 等结果保留，缺少结果不能证明没有副作用，不自动重跑或回滚。

状态文件只授予当前账户/SYSTEM 的受保护 DACL，拒绝重解析点、硬链接和外部授权 ACE。核对每行 SHA-256、序号及项目/backend/实例绑定；摘要用于损坏检测，不提供抵抗本机所有者的签名证明。忽略没有换行的末尾半条追加并保留原文件，中间损坏、完整坏行、权限不符或预算超限阻止启动。

Windows 1.0.6 每 backend 累计 256 MiB、8192 个历史逻辑记录；Mac/Linux 保持 32 MiB、256 条。各后端仍最多 64 个实例文件（第 65 次启动拒绝），单行 2 MiB，扫描最多 4096 项。分别计数，不自动淘汰或删除历史。Windows 任务回执的 persistent_state 单独报告容量；task_archive 释放已结束任务的实时记录及输出，保留精简回执及磁盘历史，不删除审计。超预算保留记录并阻止新操作；状态查询与实际任务取消仍可用。停机审阅、备份后准备新数据位置或明确清理，远程没有删除历史的接口。

## 验证

常规 scripts/test.ps1 -SkipCloseProbe 包含 M6 普通专项。remote_approval_checks.py 的 --ui 使用开发机 Node 24 与 Edge 实际显示、点击并核对结果；这些不是产品依赖。--administrator 需人工 UAC，始终使用独立测试配置/Data。独立机器、已有公网代理、另一设备及真实 MCP 客户端须另外验收；历史 HTTP close 重置仍在调查。
