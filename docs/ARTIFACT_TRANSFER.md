# 受控产物上传与下载

版本：`0.1.0-dev.18`，M11。日期：2026 年 10 月 12 日。用户确认同时实现上传和下载；本轮管理员专项等待 UAC 超时，用户随后暂不方便配合，实际管理员传输待验收。本机常规验证见 [M11 交接](HANDOFF_20261012_M11.md)。

## 使用方式

通过已认证的项目 MCP 地址使用 artifact_download / artifact_upload。当前提供编号和分块接口，没有匿名下载 URL、浏览器附件或 ChatGPT 专有附件对象；客户端须解码/组装分块后才能得到实际文件。没有加入公网 OAuth 或隧道管理。

仅正常 format_version:1 项目支持传输；旧 --config 和 legacy_config 入口不开放这两工具。将需要的名称加入 policy.tools。下载遵循既有文件读取范围与审批；上传还要求目标目录可写。manager.example.json 的只读示例只增加 artifact_download。不会因加入下载工具自动允许写入或管理员操作。

| 方向 | 流程 |
| --- | --- |
| 下载 | open 提交授权文件、request_key；批准后返回 transfer_id、size、完整 sha256。read 沿 next_offset 获取 Base64 原始字节及每块摘要；status 查看状态，cancel 终止后续读取 |
| 上传 | open 提交新目标、request_key、精确 size、expected_sha256；批准后获得 transfer_id。write 按 next_offset 追加 Base64 分块；commit 核对完整大小和摘要再创建目标；status / cancel 查看或清理 |

两工具的 action 必填。open 使用 workspace_id 和相对 path；current_user 文件范围可省略 workspace_id 使用绝对本地盘符路径。request_key 为 1–64 字符，同调用者在同一后端不能将它改用于其他参数或方向。传输 ID 为 32 位随机十六进制，不替代认证；它绑定项目、实际传输 actor 和运行后端。

## 下载示例

以下为 tools/call 参数：

```json
{"name":"artifact_download","arguments":{"action":"open","workspace_id":"workspace","path":"report.pdf","request_key":"report-download-1"}}
```

always 项目先返回 pending，所有者批准后，原调用者携带相同参数和 approval_id 重试。批准前只固定实际文件对象元数据，不读取文件内容或计算正文摘要。成功结果包含文件名、完整大小/SHA-256、transfer_id、next_offset 和分块上限。

```json
{"name":"artifact_download","arguments":{"action":"read","transfer_id":"返回的32位编号","offset":0,"max_bytes":32768}}
```

客户端对 data_base64 解码，核对 chunk_sha256，按 next_offset 继续，最后核对完整 size 和 sha256。has_more:false / status:completed 表示源读取结束；它不证明客户端已经成功保存文件。仅可重试最近一块，offset 和 max_bytes 必须相同，不能随机跳读旧块。open 可选 expected_sha256 限定预期版本。

下载保持源读句柄，阻止普通文件写入/删除共享，并核对实际对象版本；结束或取消后关闭句柄，源不修改。最近一块的有界回执可在原许可期限内重试，cancel 后不再返回缓存字节。它不是独立复制的磁盘快照，也不承诺对抗受信任同账户/管理员的任意内存映射或系统级修改。

## 上传示例

准备字节 b'hello'，大小 5，SHA-256 如下：

```json
{"name":"artifact_upload","arguments":{"action":"open","workspace_id":"workspace","path":"new.bin","request_key":"new-upload-1","size":5,"expected_sha256":"2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824"}}
```

审批绑定目标父目录对象、参数、完整大小及预期摘要。open 不创建最终文件。临时字节存于项目私有 Data 内的受保护文件，ACL 只授予当前账户与 SYSTEM，不将它放入授权工作目录。

```json
{"name":"artifact_upload","arguments":{"action":"write","transfer_id":"返回的32位编号","offset":0,"data_base64":"aGVsbG8="}}
```

```json
{"name":"artifact_upload","arguments":{"action":"commit","transfer_id":"返回的32位编号"}}
```

write 只接受标准、带正确填充的规范 Base64，无空白、URL 字母表或无效尾位。只能在 next_offset 追加；相同最近一块重试不再写入，改变该块字节拒绝。零字节文件可以直接 commit，仍须提供空内容摘要。

commit 前保持目标不存在（若调用前它已存在，则保留原文件）；接收不完整、摘要不符、目标已存在或权限失败时不覆盖。通过核验后，在已固定父目录创建临时文件、刷新并以 create-only 重命名为目标；不会创建缺失目录、不执行或解包上传内容。文件的扩展名/内容不受格式限制，不能把摘要核对当成内容可信或恶意文件检测。

成功 commit 的重试只返回原回执；外部后来修改或删除了目标，也不会重新写入。committed:true 表示该次提交曾成功，不是目标目前仍存在/仍匹配的证明。取消已提交上传不能回滚文件。

## 权限、期限与预算

always 中 open 先审批；workspace_auto / never 遵循原目录范围和读写许可。单次批准的是一份有预算的逻辑传输，不对每块重复审批。读/写/未完成的 commit 均检查原许可及有效期；撤销或取消原批准后停止继续传输。改变范围、文件版本、预期大小、摘要或参数需要新许可。账户文件范围、工具开关、执行身份和管理员许可仍各自独立。

| 项目 | 当前上限 |
| --- | --- |
| 单文件 | 64 MiB，二进制原样传输 |
| 分块 | 32768 个原始字节；Base64 最多 43692 字符，适配既有本机请求预算 |
| 活动传输 | 每个项目/后端最多 8 个 |
| 活动总量 | 声明/源大小合计最多 256 MiB，上传先预留预算 |
| 传输记录 | 每个运行后端 32 个，完成/失败/取消保留元数据，不静默淘汰幂等回执 |
| ttl_seconds | open 可选 1–900，默认 300；实际审批许可可能更早到期 |

达到记录上限后，先审阅已提交文件，再明确重启后端。当前没有断点跨重启恢复或后台定时扫描；到期在下一次传输调用时关闭句柄，退出/崩溃也由删除关闭句柄的机制清理私有 spool。下载完成关闭源，上传完成/失败/取消关闭临时文件；工作进程退出、项目暂停/停止和管理员许可更新清理对应后端，编号不能跨实例使用。

管理员操作须每次保留 identity:administrator，使用所有者明确许可的实际 UAC 代理及其独立审批池；普通批准不能授权管理员传输。普通/管理员编号不互通。实现已接入共享代理；本轮真实管理员传输尚未验证，不能引用 M10 文档专项作为 dev.18 传输验收。

正常项目沿用文件调用审计，日志记录操作与规范请求摘要，不保存二进制内容或 Base64。status / cancel 只操作本人记录，清理不因审计不可写而受阻；读写及 commit 仍需健康的审计/持久状态。传输状态和分块回执只在内存，重启不会恢复；私有历史中的 open 审批回执不代表传输仍活动。

当前只提供正常调用边界，不能隔离有任意同账户或管理员代码执行能力的其他程序。提交有磁盘 I/O 和既有通信/进程预算，但未实现独立传输后台任务、硬 I/O 超时或掉电事务保证。commit 中崩溃可能留下目标目录内的临时文件，或实际文件已提交但回执未收到；不能自动重放，需所有者核对效果。

## 配置与验证

只读下载项目可在现有 tools 中增加 artifact_download。允许上传时，使用明确可写工作目录，并增加 artifact_upload；命令执行可以继续关闭。例如 policy 片段：

```json
{"approval_mode":"always","files":{"scope":"configured"},"execution":{"enabled":false,"default_identity":"restricted","administrator_allowed":false},"tools":["host_info","host_capabilities","artifact_download","artifact_upload"]}
```

其 workspaces 中目标目录仍须 read_only:false。保存配置后按既有流程重启管理器，不修改正在运行的实例。

默认产品为 C17 / Windows 原生文件操作，没有新增运行依赖。本地测试和脚本继续按既有用户要求被 Git 忽略；持有测试副本可运行 scripts/test.ps1 的 -Transfers，同时使用 -SkipCloseProbe 保留历史 HTTP close 问题的验收边界。真实 MCP 客户端的附件显示/保存、独立系统、远端代理、大文件性能及人工管理员专项均另行验收。
