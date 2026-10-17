# ShellBot 实现与验证记录

更新日期：2026 年 10 月 17 日。源码版本：`0.1.0-dev.19`。仓库：`E:\Program\ShellBot`，分支 `main`。当前基准提交 `051bf77a87243ec7ff6bfe4ac3d69d77acb247c2`，M11 已由用户提交和同步；M12 未提交和推送。以下旧阶段段落保留当时快照，以最新交接为准。

2026-10-17 最新：按用户确认取消真实重启/注销验收要求，以 Windows 实际登记及回读设置为准。已提升管理器复用已有管理员令牌创建共享代理；普通 worker 取得实际普通令牌，普通任务和 LPAC 保持原身份。新增同账户最高权限交互登录计划任务、普通 Run/计划任务冲突保护、准确归属及读回校验。服务状态增加实际自动/延迟启动及虚拟账户查询。使用与限制见 [启动说明](STARTUP.md)。

Debug / Release 各 14 份报告分别通过 7062 / 7068 项，摘要绑定最终程序；桌面各 390 项，启动各 124 项。Release 真实管理员与计划任务专项 307 项、被修改计划任务保护 10 项通过。用户未留意最后一轮 UAC 次数，不称为人工确认只有一次。旧专用测试服务、Run 项和最高权限测试任务已移除，五个测试对象原 DACL 逐字节恢复，无关 Run 项不变，独立清理 13 项通过。初次 ACL 恢复差异和开发期间失败报告保持原结果。总收据 build/validation/m12-elevated-startup-acceptance-20261017.json，当前摘要及完整证据见 [最新交接](HANDOFF_20261012_M12.md)。

2026-10-17 较早补验：真实 UAC 安全桌面时的新控制申请、观察、截图及输入均拒绝，返回后旧租约仍失效；用户确认屏幕变暗并等待约 15 秒后允许。实际高完整性管理员窗口的五类输入与窗口切换均按权限拒绝，普通窗口对照输入成功。独立管理员窗口专项 78 项通过，安全桌面子项来自另一份保留整体失败状态的真实报告。该轮对应旧 dc21316a 程序，不能替代新摘要的完整安全桌面人工重跑，分项见 [人工验收记录](MANUAL_ACCEPTANCE_20261012.md)。

## 2026-10-12 人工验收与服务身份修复

恢复后实际 Explorer 重启暴露桌面控制图标丢失；仅 desktop.c 新增任务栏重建登记和有界重试，Debug / Release 桌面各 390 项及各 13 份普通回归通过，用户实际复测两个图标和菜单恢复。独立测试服务已更新到最新 Release（dc21316a），实际运行与防重放复测通过，当前等待用户真实重启取得登录/未登录启动证据；最新摘要和操作见 [人工验收记录](MANUAL_ACCEPTANCE_20261012.md)。旧实际管理员报告继续保留原摘要，未改写为最新程序报告。

用户已允许补齐此前人工验证。实际虚拟账户测得 TokenElevation:true、Session 0、管理员组未启用；修正 service-run、服务管理器/worker 及受限任务初始化对该合法令牌的误拒绝。准入仍核验准确 ShellBotService SID、Session 0、管理员组/限制/AppContainer，普通入口不放宽；受限子任务独立验证未提升 LPAC，父/子诊断如实区分。已验证 SCM 运行、独立 owner 审批、只读下载、保护目录上传及真实受限任务。旧源码、程序和人工报告保留在 build/manual-before-service-token-fix-20261012。后续人工进展、报告与系统登记清理见 [人工验收记录](MANUAL_ACCEPTANCE_20261012.md)；以下 M12 段落保留开发轮历史。

## dev.19 M12 服务与托盘常驻（开发轮历史）

按用户最新决定同时提供 Windows 服务和当前用户托盘模式。启动文件保存私有 DPAPI 凭据及完整项目配置快照，选定项目仅启动一次，不恢复旧许可或重放任务。新增原生 ShellBot-Launcher.exe、Windows 服务控制接口、虚拟服务账户和 Session 0 身份核验；服务限制本地 configured 范围、独立所有者、无桌面/管理员及账户任意命令。托盘隐藏/打开、任务继续及明确退出复用原管理器，原 --manager/--manage 行为保留，见 [常驻说明](STARTUP.md)。

Debug / Release 均通过 `/W4 /WX`，最终 26 份常规报告匹配主程序，合计 6674 / 6663 项，M12 各 105 项。实际验证加密且私有的配置、无终端凭据时的真实 HTTP 文件工具、冻结配置、仅选定项目启动、托盘注册回执/显隐、原生任务继续与退出、无控制台相邻启动器、重复 Data 拒绝、服务策略/普通安装/角色拒绝及前台诊断。没有把前台 service-mode 诊断当作实际 SCM/虚拟身份验收。

首轮登录登记写入 Windows API 未返回，20 秒测试超时；固定阶段诊断定位到 RegSetValueExW，未确认环境根因，用户界面提示的询问尚未获得回复。改为本产品独立辅助进程和 Job、20 秒预算，设置在后台处理；最终同一环境约 23.19 秒返回 exit 2/“状态未确认”，测试原启动项完全保留、无残留，不当作已登记成功。另保留夹具换行误期望、启动器图标就绪时序失败与修正，以及已确认测试进程未完全退出时的链接占用错误。

实际服务安装/启动/停止/卸载、虚拟账户目录权限、未登录开机、真实登录启动和物理托盘点击待人工验收，本轮未触发 UAC、安装服务、重启或改变真实用户配置。保留 143 文件 dev.18 基线、10 个原产物、24 份原普通报告和全部 11 个旧 ZIP；测试继续仅本地保留并忽略。最终源码/测试清单、报告索引、收据及继续验证见 [M12 交接](HANDOFF_20261012_M12.md)。

## dev.18 M11 受控产物上传与下载

按用户确认同时实现 artifact_download / artifact_upload，仅开放正常新格式项目。单文件最多 64 MiB、原始块最多 32 KiB，下载返回真实二进制的 Base64、完整及每块摘要；上传批准后在私有 Data 暂存，核对准确大小和 SHA-256 后在固定父目录 create-only 创建，不覆盖、不执行上传内容。沿 next_offset 继续、仅最近块可幂等重试；成功提交回执不重复写入。32 条记录、8 个活动传输及 256 MiB 声明总量各自有界。

接入统一文件范围、只读、工具掩码、open 审批、actor、策略版本、实例及普通/管理员后端归属。open 完成后的原许可仍可撤销，后续内容操作检查期限与许可；取消、到期调用、项目停止/重启释放私有暂存，重启不续传。日志不保存 Base64 或二进制内容，status/cancel 可在审计故障时清理本人记录。项目工具数增至 32，旧入口仍隐藏传输；普通传输不要求命令或桌面权限。没有新增运行依赖、匿名下载 URL 或客户端附件集成，见 [产物传输](ARTIFACT_TRANSFER.md)。

Debug / Release 通过 `/W4 /WX` 构建，24 份最终常规报告匹配程序摘要，分别合计 7624 / 6521 项；传输各 1015 项。实际 authenticated HTTP 多 MiB 上传/下载独立核对字节，覆盖只读、跨项目/actor/方向、摘要、审批撤销、最近块重试、取消/过期/重启、8 活动上限、256 MiB 预留及 32 完成记录不淘汰；大文件吞吐没有据此验收。核心首轮 Release 在旧 fs_patch 首申请缺少 approval_id，原报告保留；同一程序串行复测及 20 次独立读后申请均通过，首轮未保存具体工具错误，根因尚未确认，未宣称修复。

早一轮管理员专项在 30 秒等待后未确认代理启动，969 项报告 passed:false；没有运行保护文件/目录的 ACL 步骤。测试等待改为 300 秒后尚未人工重跑。用户随后明确暂不方便配合，继续完成其余开发和常规验证，本轮不再触发 UAC；真实管理员传输待验收，旧 M10 的管理员文档通过记录不替代当前验收。

开始时保存 140 个跟踪文件的 dev.17 回退基线，原 10 个产物、22 份普通报告、旧管理员文档报告及 11 个 ZIP 保留核对。本地测试和脚本继续被 Git 忽略。当前源码/测试清单、初始报告索引及常规验收收据位于 build/validation/m11-*.json；未修改实际 Data/配置/隧道，未提交、推送或打包。后续人工验证、限制和摘要见 [M11 交接](HANDOFF_20261012_M11.md)。

## dev.17 M10 PDF / Office 阅读与预览

按用户确认优先 DOCX / XLSX / PPTX，新增 document_info、document_read、document_preview。PDF 使用固定 PDFium 157.0.8086.0 原生 DLL，读取真实页/文字并渲染 PNG；Office 使用 Windows OPC / XmlLite，按关系读取正文、工作表存储值及幻灯片文本。Office 预览明确标注为结构化内容预览，不复现原始排版；旧格式/宏格式提示转换，无 OCR、公式重算或外部引用执行。产品不依赖 Office、Python、Node.js 或 .NET，完整边界见 [文档阅读](DOCUMENT_READING.md)。

文件范围、工具掩码、只读、审批、真实身份及管理员许可沿用既有管线；批准前不解析文档。源最多 32 MiB，单元文字 1 MiB、每页 64 KiB，预览 PNG 2 MiB；Office 部件、展开总量、XML 深度/事件和共享字符串另设预算。修正桌面工具范围错误、XmlLite 结束节点深度、共享字符串缓冲复用及 document_preview 工具名存储过短的问题；正常项目的工具说明保留格式语义、预览限制和统一权限说明。

Debug / Release 均通过 `/W4 /WX` 构建。22 份最终常规报告匹配实际程序，合计分别 5536 / 5527 项；其中文档各 258 项、PDF 依赖缺失/篡改/有效及 Office 独立运行各 14 项、图像各 681 项。最终 Release 实际 UAC 文档专项 355 项通过，独立测得管理员令牌，并读取/预览只有管理员和 SYSTEM 可读的 PDF / DOCX，原 DACL 恢复、源不变、代理正常停止。UAC 次数未由自动化观察，当前专项只覆盖文档路径，不替代旧管理员全生命周期或桌面输入验收。

保存 108 文件 dev.16 回退基线，原程序/报告及 11 个旧 ZIP 保留核对。测试源码/脚本继续仅在开发机保留并由 Git 忽略，失败报告和修正工具说明前的程序/报告均保留。未改变实际配置、Data、凭据或隧道，未制作新包、提交或推送；历史 HTTP close 重置、独立系统和真实 MCP 客户端仍待验收。完整摘要、报告和下一阶段 M11 受控产物传输见 [M10 交接](HANDOFF_20261011_M10.md)。

## dev.16 M9 图像完整解码与缩放

`image_view` 实际解码全部 PNG/JPEG 像素，并可按 max_dimension 保持比例缩小，较大源自动转为有界 PNG。输入限制 16 MiB、16,777,216 像素、32,768 单边，输出不超过 2 MiB；文件、目录和审批权限不变，许可增加尺寸参数绑定并按源上限计量。原文件不修改；无需处理时保留原字节，衍生图不复制 EXIF/ICC/文本元数据，不应用自动旋转。

实际测试发现 Windows 解码器会补全缺失 PNG 像素行；补充有界 DEFLATE、精确行数据长度、Adler-32、过滤类型与 Adam7 检查后明确拒绝。独立 Pillow 核对真实输出像素、透明度、尺寸与源不变，Debug/Release 图像专项及旧功能回归通过，最终结果见 [M9 交接](HANDOFF_20261011_M9.md)。本轮不触发 UAC、桌面或剪贴板操作，M8 人工输入专项不重跑，旧专项不当作 dev.16 验收；保留独立系统、真实客户端和当前提权待办，未制作新包。

## dev.15 M8 Windows 桌面工具

新增十二个项目工具：短期独占租约、桌面状态/PNG 截图、窗口列表/切换、点击/拖动/滚动、Unicode 输入/组合键和纯文本剪贴板。观察、输入、剪贴板读写分别授权，默认关闭，不因文件范围或任务身份自动扩权。接入原审批模式、参数绑定、结果重试和脱敏历史；屏幕、窗口标题、输入及剪贴板正文不写入审计或恢复日志。

同账户/会话跨管理器使用受保护内核对象，控制绑定项目、认证调用身份及工作进程实例。人工及外部模拟输入触发全局暂停；默认 Ctrl+Alt+F9 实际注册后才开放控制，只能通过托盘确认或本机管理命令恢复，重启项目不消除管理器持有的暂停。过期观察、前台/布局变化、锁屏/断开、未知或提升目标均拒绝输入。工作进程退出释放锁和钩子，不自动重放。

Debug/Release 严格构建通过，两份 M8 原生窗口专项各通过 383 项，覆盖真实 PNG 像素、Unicode/表情、组合键、点击/拖动/滚动、焦点切换、参数重试、跨项目/跨管理器排他、暂停/停止、完整剪贴板恢复、过期和进程异常退出。首次检查发现并修复鼠标 dwExtraInfo 被 Windows 截为 32 位导致自身事件误判的问题；异步前台切换增加有界确认。已有文件复制剪贴板因延迟格式不能完整备份时，测试在任何覆盖之前停止；用户自行改为普通文本后完成写入和恢复验证。

Debug/Release 的 M1–M7 和核心回归均通过，加上 M8 分别为 5,065 和 5,137 项检查；计数包含轮询和文件扫描，不是固定功能数量。最终完整结果、程序摘要和保留的人工验收项见 [M8 交接](HANDOFF_20261011_M8.md)。本轮不请求 UAC，专用窗口与热键注入检查不替代人工托盘、真实物理停止、多显示器/缩放、锁屏/RDP、独立系统或实际 MCP 客户端验收。原 dev.14 候选包和 dev.7–14 基线全部保留，本轮不制作新候选包。

M7 开始时的基准提交为 `31ce8568a8a1e49a07a8eb748f2683a398182735`，当时 M6 已由用户提交，本地 origin/main 与 HEAD 一致、未查询远端。M7 当时为未提交本地更改，保存 build/baseline-dev13-m7-20261011 的全部 115 个跟踪文件、原 out 和验证报告，保留全部旧基线与包。后续 M7 与维护更改已由用户提交为 747ec63；实际 Data、配置和隧道未修改。

## 2026 年 10 月 11 日使用说明与仓库维护

M1–M7 的本机开发和候选验证已完成，整个计划仍包含独立系统、另一设备/代理、真实 MCP 客户端验收和后续桌面、文档等功能；历史 HTTP close 问题仍在调查。新增 [使用说明](USER_GUIDE.md)，README 提供入口。

按用户要求忽略整个 tests 目录和四个测试/诊断专用脚本，从 Git 索引移除原有 25 个文件，保留全部 27 个本地测试文件及维护备份。默认 build.ps1 只构建产品，-WithTests 仅供拥有本地测试副本的开发者使用；产品 SandboxProbe 源码、工程和构建仍保留。复制 97 个产品源码/文档文件到不含 tests 的独立目录，实际 Release 构建、版本查询和示例配置校验通过，没有请求 UAC。首次工具沙箱内链接遇到 MSPDB140.DLL/LNK1101，在同一隔离副本、工具沙箱外重新构建通过；未替换原 out 程序。

删除 build/x64 编译中间文件和 tests/__pycache__：167 个文件、45,202,858 字节。IDE SemanticSymbols.db 的独占读取返回占用错误，第一次检查未删除任何文件，因此保留整个 .vs 及布局设置；验证用临时源码副本另行删除。核对原 out、所有 dist 包、基线和已有验证报告共 1,852 个文件，SHA-256 全部保持一致。收据为 build/validation/housekeeping-20261011.json，测试文件与原索引备份在 build/housekeeping-backup-20261011-171908。

原 M7 候选包与验收收据继续对应维护前的 123 文件源码快照，未修改其内容或摘要。本次文档、构建及 Git 排除规则改变了源码清单；如需给新快照打包，须生成新的验收证据，不能复用旧源码摘要。本次未提交或推送。

## dev.14 M7 本机管理窗口与候选验证

新增原生 --manage 窗口及本机 management-status，通过原受保护所有者通道显示项目状态、地址、文件范围、审批、身份、管理员许可及实际诊断。后台线程执行有界请求；关闭窗口保留管理器，不重发有副作用请求。新增暂停/恢复：结束本人普通/管理员任务，阻止新 MCP 操作，明确恢复创建新实例，历史与旧任务键不重放；暂停仅本管理器有效。

实际原生窗口验证项目启停、暂停/恢复、诊断和选择，人工 UAC 另验证窗口启动/停止管理员代理及 A/B 任务归属。新增候选打包器要求当前源码、程序、探针、M1–M7 及实际提权收据，不改旧打包器。解压后使用外部开发夹具验证实际程序，不把夹具作为产品依赖。摘要与验收边界见 [M7 交接](HANDOFF_20261011_M7.md)。

以下 M6 及更早内容保留当时实现和验证范围；历史提交/下一步说明不代表当前状态。

## dev.13 M6 独立所有者审批与持久恢复

新增可选独立 owner_token_env、受保护的有限审批 API 和实际浏览器页面；项目 bearer 不能批准，所有者凭据不进入 worker/agent/task 环境。决策核对规范摘要、实例与策略版本，审计主体由可信入口设置。客户端只有声明 2026 URL 能力才收到 input_required；accept 不授权，旧/不支持客户端使用 pending 链接。增加本人 approval_status/approval_cancel，普通和管理员审批池保持隔离。

正常项目新增有界 state 日志，固定管理员 Data/administrator-state，任务和输入在副作用前刷新意图。保存有限真实输出、输入投递回执及自然完成/取消/期限/输出限额事件。新实例只加载历史并中断旧许可，不恢复执行或重投 stdin；完成结果保留，缺少结果不推断成功。损坏、权限不符和预算超限阻止新操作，旧记录不自动淘汰。具体 API、预算与运行边界见 [M6](REMOTE_APPROVALS.md)。

验证使用独立 build/m6-tests：HTTP 认证、参数绑定、实际 Edge 页面显示与点击、异常退出/重启、半条追加、损坏摘要、公开 ACL、32 MiB 和 256 记录预算，及人工 UAC 后的真实管理员任务/历史。最终摘要与报告链接见 [M6 交接](HANDOFF_20261010_M6.md)。当前文件系统不支持硬链接对照，明确 skipped；独立 Windows/Server、另一设备、已有公网代理和真实 MCP 客户端待验收。历史 HTTP close 重置未宣称修复，仍使用核心回归另记。

以下 dev.12 及更早部分为当时证据，版本限制和提交状态不作为当前状态。

## dev.12 M5 共享管理员代理

普通管理器通过本机所有者 administrator-start 异步请求 UAC，administrator-status 返回启动中、实际代理身份或启动错误；等待人工操作不占用项目/管理员请求锁。重复启动和查询不重复请求 UAC，不自动重启失效代理。普通工作进程和默认任务保持普通权限；代理只接受同账户、同会话的实际提升。新项目显式 identity:administrator 才使用代理中的文件与任务后端，旧迁移和 LPAC/目录隔离项目继续拒绝。

新增 shared_admin.c 独立校验管理器可信注册、普通 worker 的实际令牌及持续持有的进程句柄。每次操作验证项目、策略/授权版本、实例、主体、摘要、序号和期限；工具策略及审批池在代理内独立执行，不接受 approved=true。普通 worker 的转发必须与已认证原请求完全相同；管理器选定 manager-owner 或 project-mcp。代理通道不向 worker/task 继承，子任务环境去掉项目凭据。

管理员文件访问复用六工具的实际句柄、只读范围、审批、冲突、私有备份和重试逻辑；管理员任务沿用原生进程、Job、输入输出、期限和归属检查。各项目 Data 下固定 administrator-实例 子目录，保存独立审批/任务/幂等/审计。allow/revoke 更新当前管理器授权版本且不写回被固定的注册配置；撤销失效旧批准并取消该项目活动管理员任务。项目停止及 worker 崩溃只取消其所属管理员任务，管理器停止和代理死亡回收全部相关 Job。

普通与管理员后端的任务、审批等配额分别有界，尚未合并；授权和请求/任务恢复仍为内存状态。管理员与普通账户任意程序保留操作系统权限，不能因此宣称项目之间抵抗管理员干扰。具体使用、取消启动和配置持久化边界见 [共享管理员代理](SHARED_ADMINISTRATOR.md)。

实际验证修复了两个产品问题：命令 cwd 版本原先读取目录所有内容，管理员保护文件导致同目录普通任务也无法启动，现改为实际目录对象身份/元数据版本；程序文件仍固定版本和内容摘要。启动 UAC 原先同步占用管理器锁，超过所有者 30 秒等待并拖住退出，现独立启动上下文立即返回并轮询状态，退出时取消发布；晚到代理不能向已退出管理器注册项目。测试异常清理也确保保存原始失败报告。

| 最终 M5 本机检查 | Debug | Release |
| --- | --- | --- |
| M1 配置/策略（含 151 项原生策略） | 428 | 428 |
| M2 实际项目进程 | 224 | 224 |
| M3 实际 HTTP 网关 | 336 | 336 |
| M4 审批及 LPAC | 584 | 584 |
| M5 不请求 UAC | 97 | 97 |
| M5 实际提升与共享代理 | 434 | 429 |
| 旧核心，排除 close/UAC | 2427 | 2420 |
| 单独取消 UAC | 未重复 | 126 |
| 两个管理员任务活动时停止管理器 | 未重复 | 156 |
| 总计 | 4530 | 4800 |

16 份最终报告与主程序摘要匹配；Debug SHA-256 为 2415bfd181c59c4a93b21241a4914dddae1403347636fc76ae140cfae5691478，Release 为 b642d82a363ee1a1acfa59d60b6915afeec0c79e2a35dc5ef02ea33d1f0feeba。计数包含轮询和保存文件扫描，不能视为固定功能数量。所有运行只使用独立测试配置、Data 和原生辅助程序，实际用户配置、Data 和隧道未修改。完整摘要、基线核验、失败保留及下一步见 [M5 交接](HANDOFF_20261010_M5.md)和 build/validation/m5-local-acceptance.json。

人工协作完成实际提升及拒绝启动。取消测试两次因用户确认点击“是”而未满足取消预期，报告保留；最终独立取消取得操作系统 cancelled 错误且无自动重试。脚本仍记录 uac_dialog_count:not_observed_automatically，不根据启动尝试冒充用户观察的总弹窗次数。早期测试/夹具/断言问题及同步等待失败也保留，原始报告缺失的那轮明确标记为工具诊断摘要。

下一项为 M6 独立远程所有者审批、客户端交互适配及请求/任务持久恢复。Windows 10、Server 2025、其他账户及真实 MCP 客户端须另行验收；跨账户共享提权当前明确拒绝。历史 HTTP close 重置仍未解决；核心排除 close、通用旧 UAC 和不可用硬链接夹具，M5 提权由独立专项证明。本轮未提交、推送或打包。

## dev.11 M4 全工具审批与实际受限执行（历史基线）

正常项目的 fs_list/read/search、image_view、fs_write/patch 和 task_start/input 统一接入策略与原审批队列；沿用本机 project-owner 查看、批准、拒绝、撤销及列表。always 在批准前仅返回元数据和待办，没有文件正文、搜索片段、图像或副作用；workspace_auto 按工作目录/额外目录分类，never 仍执行只读、工具、范围及身份上限。HTTP project-mcp 与 manager-owner 的批准按 actor 隔离。当前账户文件范围新增绝对本地盘符路径适配，按物理父目录保留显式只读覆盖，不能分类时拒绝。

许可绑定项目、策略、实例、调用者、规范参数、对象身份/版本、期限和预算；执行前重新核验。读取分页按 next_offset 延续原许可，总量为 1 MiB 文本或 1000 个目录项；同页可重试，跳页及目标变化拒绝。大图像超缓存时在原版本/期限内重读，预算只计一次。自动纯读取不占审批记录；修改保留完成回执和原备份，拒绝重复副作用。记录不自动淘汰；用尽后需要所有者复核并明确重启。创建禁止覆盖；修改在读原文前独占核验、检查 expected_sha256、保存私有备份，仍不是崩溃原子保存。

新增 Windows LPAC 后端：每工作进程独立 AppContainer profile/SID、零 capabilities、退出 ALL APPLICATION PACKAGES 授权、禁止 Win32k 和子进程、单进程 Job。实际 token 与 AccessCheck 对照及原生行为探针全部通过才开放任务；不足条件时保持不可用，文件工具可继续使用。TokenIsLessPrivilegedAppContainer 查询在开发机返回 ERROR_INVALID_PARAMETER，不把不支持的查询误当成证据。该后端开放精确声明的 readonly runtime_files，最多 16 个程序/DLL/资源，固定其读句柄；禁止管理 Data 作为运行资源。主工程会构建旁置产品依赖 ShellBot-SandboxProbe.exe，原生开发夹具不是产品依赖。

批准后钉住实际程序，通过有界流式 SHA-256 将程序内容摘要绑定任务 request_key；原任务/输入幂等逻辑及归属、输出、取消、期限与环境允许列表保留。目录 ACL 初次授权最多 1024 个对象/16 层，不包含重解析点或多硬链接文件，不授予 WRITE_DAC/WRITE_OWNER。活动注册配置文件被钉住，禁止写入与替换；所有者可导出新配置再明确重启。

每次 ACL 变更前刷新受保护的恢复日志，记录 profile/SID、物理路径及对象标识，没有内容和凭据。正常停止先杀本人 Job，再清理该实例 SID 授权和 profile；清理包括任务新建文件，对联接只处理链接对象。原 owner/group/其他 ACL 保留。管理器异常退出后，在显式启动前恢复，成功保存受保护的 .done 回执；清理失败状态 cleanup_required 阻止新启动。恢复不重跑操作，也不恢复批准和任务。

完整测试统计、程序摘要及最终证据见 [M4 交接](HANDOFF_20261010_M4.md)、build/validation/m4-local-acceptance.json 和 m4-source-manifest.json。x64 Debug/Release 的报告为 x64-{Debug,Release}-{m1,m2,m3,m4,core}.json，各报告绑定对应程序 SHA-256。M4 固定原生程序对照实际普通账户与 LPAC：目录外读取、只读写入、管理日志、改 ACL、所有者管道、父进程注入、注册表、网络、子进程和 Win32k；另测运行期间新增联接的外部目标拒绝、下次启动已有联接拒绝、停止恢复原 ACL 和新文件授权、明确重启实测。64 KiB HTTP 写入及超缓存图像重试也通过；保留测试文件不含 bearer 值。

初次通过额外 Windows PowerShell 宿主运行时，旧 M2 联接夹具创建曾被拒绝，直接运行通过，改为当前宿主执行全流程后不再阻塞；宿主相关性是本机观察，不作为根因结论。新增配置写保护使旧“运行时改注册文件”测试按预期被拒绝，M2/M3 断言同步为写保护及原策略/路由保持有效。核心初次因测试仍断言 dev.10 失败，更新至 dev.11 后通过。独立失败目录保留，早期汇总报告由最终报告替换；build/validation/m4-initial-validation-failures.json 保存工具输出的诊断摘要，明确不是原始报告副本。最终复核补齐已完成读取许可的撤销/到期、实际项目审批模式与配额报告，再对新程序完整回归。构建审批两次出现连接中断、操作未执行，经相同审批流程重试完成；没有绕过审查、降低安全断言、关闭编译警告或修改系统工具链。

用户已允许必要人工操作，M4 不需要 UAC，未运行管理员专项。核心继续跳过管理员启动、开发机不可用的硬链接夹具和历史 close 诊断，不把 skipped 计为通过。LPAC 初期只支持兼容已声明资源和无网络/子进程/Win32k 的原生程序，不宣称通用 PowerShell、GUI、构建工具或所有 Windows 版本兼容；它不是只暴露 cwd 的虚拟机，也不隔离已获账户/管理员权限的程序。M5 共享管理员代理、M6 远程审批与持久任务、独立 Windows/Server、真实 MCP 客户端及已有公网隧道验收仍待完成。本轮未改实际配置、用户 Data 或隧道，未提交、推送或打包。

| 最终 M4 本机检查 | Debug | Release |
| --- | --- | --- |
| M1 配置/策略 | 418 | 418 |
| M2 实际项目进程 | 224 | 222 |
| M3 实际 HTTP 网关 | 333 | 336 |
| M4 审批及 LPAC | 584 | 587 |
| 旧核心，排除 close/UAC | 2418 | 2418 |
| 总计 | 3977 | 3981 |

十份报告均匹配最终主程序，M1 原生策略程序摘要也已核对；解决方案 x64 Release 增量构建通过且程序摘要未改变。dev.10 回退的 96 个 Git 文件与基准提交逐个 blob 一致，原 Debug/Release 与原回执一致，四份旧包摘要核对通过。当前源码/文档清单为 103 文件，最终未发现主程序或原生探针/夹具残留。计数差异包含时序轮询及保留文件数量。

## dev.10 M3 项目 MCP 地址与认证（历史基线）

普通管理器新增可选 --http，仍共用回环端口 8765。每项目 /projects/ID/mcp 和 /projects/ID/healthz 只接受该项目独立 bearer；/mcp、/healthz 只有显式 default_project_id 时绑定该项目，不通过客户端参数选择项目。缺失、无效或重复令牌阻止整个网关启动；凭据只进入管理器缓冲，不进入工作进程消息、子进程环境、生成地址或日志。--project-links 返回本机及可选公共地址，不探测域名或更改已有隧道，见 [项目网关](PROJECT_GATEWAY.md)。

网关与旧单主机入口复用 HTTP 校验，保留协议版本、方法/工具请求头、base64 哨兵及关键重复字段拒绝。管理器转发真实请求头，工作进程按无会话 HTTP 校验，保留 JSON 响应与空 202；本机管理请求仍按原 stdio 语义处理。工作进程固定 project/revision，HTTP actor 为 project-mcp，与 manager-owner 的任务、幂等键及审批归属分开。MCP 没有管理或批准接口。

固定四个 HTTP 线程、四个应用层等待连接位置；每项目认证后先预留工作进程，再读正文。同项目忙时返回 429，不在项目侧排队，其他项目可独立响应。HTTP 正文和结果沿用 1/4 MiB 上限；私有请求另有 64 KiB 封装预算，工作进程通信共用 8 秒期限，断线或超时清理本人 Job、返回 503 和可能已有副作用提示，不自动重启/重发。旧连接保持及有界网络等待保留，历史 Connection: close 问题未修复。

| 最终本机验证 | 结果与报告 |
| --- | --- |
| Debug / Release 构建 | C17 /W4 /WX，主程序与两个原生测试程序通过 |
| Debug / Release M1 回归 | 各 405 项，build/validation/x64-{Debug,Release}-m1.json |
| Debug / Release M2 回归 | 各 220 项，build/validation/x64-{Debug,Release}-m2.json |
| Debug / Release M3 | 各 335 项，build/validation/x64-{Debug,Release}-m3.json |
| Debug 既有核心 | 2427 项，build/validation/x64-Debug-core.json |
| Release 既有核心 | 2418 项，build/validation/x64-Release-core.json |

M3 使用隔离配置和真实网关/普通工作进程验证跨项目令牌拒绝、路由和请求参数不能改绑定、同名 workspace、工具开关、协议头/通知/旧初始化、慢正文并发、重复任务/输入不重复副作用、跨项目及跨 actor 任务拒绝、迁移修改审批及重试、工作进程通信超时、显式重启、默认项目和无 public_url 连接。凭据在内存中随机生成，保留文件逐一确认不含令牌值；检查数包含时序轮询及产物数量，不作为固定功能数量。

最终 Debug 为 2570240 字节，SHA-256 `450008862bff23737d97d245a815cfaee08d64908dba854a8c22f0c769f07bcd`；Release 为 699392 字节，SHA-256 `976df32ef4f779ff8f1781fd50b2253191a52486b07f4a0419f29f16825b199a`。摘要匹配八份最终报告，汇总及来源记录为 build/validation/m3-local-acceptance.json、m3-source-manifest.json。完整 dev.9 源码、文档、程序及原报告保存于 build/baseline-dev9-m3-20261010；失败测试目录保留在 build/m3-tests。测试结束后未发现主程序或原生夹具残留进程。

用户已回来并明确恢复必要 UAC 等人工操作授权；M3 本身不需要提权测试，未启动管理员专项。核心回归明确排除历史 close 诊断、管理员启动及当前文件系统不可用的硬链接夹具，不将 skipped 当作通过。沙箱内调试链接再次出现 MSPDB140.DLL 加载错误，经工具审批后的相同本机构建通过，不改系统工具链或安全设置。

下一项为 M4 完整审批与受限执行；新项目自动写入、未列目录账户文件后端、完整审批适配和 restricted 命令仍不可用，M5 才接可信项目管理员代理。独立 Windows/Server 2025、实际 MCP 客户端、已有公网隧道和 dev.10 UAC 验收均未完成；本机网关通过不替代它们。本轮未改实际配置、Data、隧道，未提交、推送、发布或降低旧打包验收门槛。

## dev.9 M2 管理器与项目工作进程（历史基线）

新增 WinShellBot/platform/manager.c 与 manager.h，复用同一 asInvoker 程序的管理器和工作进程角色。项目创建和配置导出新文件及原字节私有备份，配置更新由程序递增策略版本；管理器固定启动快照，本机所有者可启停和查询项目，方法见 [项目管理器](PROJECT_MANAGER.md)。

每项目独立普通进程、Data 对象身份与实例锁、任务及幂等空间、审批队列、备份和审计。私有继承通信只传本项目快照，双方核验管理进程及同一程序；工作进程的句柄继承清单与环境允许列表排除其他管理句柄、全部项目凭据变量和未知环境变量。管理器固定四个所有者服务线程，同项目忙时拒绝排队，工作进程通信有 8 秒期限，断线或超时不自动重发操作。项目任务并发、记录、输出、期限、审批记录和整个 Job 内存预算已接入。

工作进程绑定 project、actor、policy_revision，实际任务后端确认归属；模型参数不能改上下文。文件工具在读取正文前判断策略，并沿用实际句柄路径核验。never 的已配置目录读取与普通账户命令、workspace_auto 的工作目录读取可用；always 及额外目录待审批读取拒绝正文访问，正常新项目自动写入、未列目录的账户文件适配、完整受限执行及项目管理员后端继续不可用。迁移项目保留普通文件读取、原审批修改和私有备份，旧 administrator 配置不触发 UAC、不降级运行管理员命令。

管理器先保存 build/baseline-dev8-m2-20261010，包含 M1 源码、文档、程序和报告。独立测试 Data 与回执位于 build/m2-tests 的各轮目录，原始失败目录保留。未改用户实际配置、Data、隧道或系统安全设置。启动期间创建的 Data、同目录 alias、卷/文件标识及管理目录重叠会被核验；配置目录范围不公开管理状态目录。普通账户任意程序的操作系统权限不因进程拆分而被隔离。

| 最终本机验证 | 结果与报告 |
| --- | --- |
| Debug / Release 构建 | C17 /W4 /WX，主程序及两个原生测试程序通过 |
| Debug / Release M1 回归 | 各 405 项，build/baseline-dev9-m3-20261010/validation/x64-{Debug,Release}-m1.json |
| Debug / Release M2 | 各 220 项，build/baseline-dev9-m3-20261010/validation/x64-{Debug,Release}-m2.json |
| Debug 既有核心 | 2421 项，build/baseline-dev9-m3-20261010/validation/x64-Debug-core.json |
| Release 既有核心 | 2403 项，build/baseline-dev9-m3-20261010/validation/x64-Release-core.json |

M2 使用实际进程验证五项目运行、同名 workspace 和幂等键、跨项目任务编号与实例拒绝、凭据环境排除、策略模式与工具开关、审批和备份各自存储、任务预算、项目停止/崩溃、管理器正常退出/强制结束及无关进程保留。全部报告摘要匹配最终程序；断言数包含时序轮询，因此不同轮次核心数量可不同。

最终 Debug 为 2551296 字节，SHA-256 `8964c59be4f586023697bef42e3092302be9f386b6e35f1ab8deb0be76bc7f80`；Release 为 687616 字节，SHA-256 `92f7ec1e3d9764efd4ccd6afeb4bdbd4cd425c01532e311376a6e3c67dcbc343`。

用户明确要求离开期间不做 UAC 或其他人工测试，本轮未运行。核心回归继续跳过管理员启动授权、开发机不支持的硬链接夹具及历史 Connection: close 诊断；这些不是通过项。独立 Windows/Server 2025、真实 MCP 客户端和管理员后端仍未验收。M3 项目 HTTP 路由/认证、M4 完整审批/受限执行、M5 共享管理员代理保持后续范围；本轮没有启动任何公网或域名验证。

## dev.8 M1 项目配置与统一权限策略

2026 年 10 月 10 日从 HANDOFF_20261009.md 继续，核对 main、HEAD、未提交目录与旧产物摘要均与交接一致，未发现正在运行的 WinShellBot/原生夹具进程。没有 AGENTS.md 附加约定。先保存 build/baseline-dev7-m1-20261010，包含原源码、文档、Debug/Release 程序、原生夹具及验证记录；旧配置、Data 和 ZIP 不替换。

新增 sb_projects.h、core/config/projects.c 和 core/policy/project_policy.c，复用既有严格配置检查与解析器。format_version:1 注册配置支持最多 16 个项目，各自独立的目录授权、Data 位置、凭据引用、策略版本、工具开关和审批/审计预算；同一配置可描述完整权限和工作目录受限项目。unknown/duplicate/type/NUL 校验、项目和目录 id、工作目录引用、审批枚举、独立身份/管理员许可、目录授权重叠、Data 冲突及凭据环境变量大小写重复均拒绝。字段详情和样例见 [PROJECT_CONFIGURATION.md](PROJECT_CONFIGURATION.md)及 config/projects.example.json。

统一判断返回 allow/deny/pending，要求可信 context 的项目、actor、策略版本及实际目标绑定。never 不扩展文件范围、只读权限、工具能力或管理员许可；workspace_auto 只自动允许工作目录内已授权文件操作。命令和输入依审批方式判断，任务结果/取消必须先核验任务属于本项目 actor，不重复审批。sb_host、sb_request_context 加入未来可信项目绑定位置，现有传输不从模型参数填入。

受限命令仍返回 restricted_backend_unverified，配置目录范围配 current_user 任意命令返回 directory_isolation_unavailable。正常新项目的全文件范围只读或含只读目录时，普通任意命令返回 read_only_command_isolation_unavailable，不以 cwd 或审批假装能够强制只读。项目管理员许可与可用后端分开；新配置管理员后端固定不可用，不能使用旧单实例代理假装完成逐项目授权。

增加 --validate-projects 和 --migrate-config 本机入口，前者只校验，后者生成新配置与原字节私有备份，不覆盖已有文件、不迁移旧 Data。dev.7 的读取自动、写入审批、账户命令预授权保存在明确的 legacy_config 兼容封装中，不静默映射为 never 或全盘文件访问；普通配置字段不能覆盖兼容封装。administrator 来源不降为普通身份执行。新版本化配置使用 --config 会在初始化执行身份、审批和 Data 之前拒绝；原未版本化单实例仍沿用旧行为。

| 最终验证 | 结果与报告 |
| --- | --- |
| Debug / Release 构建 | C17，/W4 /WX；主程序、原生任务夹具及新策略测试程序均通过 |
| Debug M1 | passed，405 项（原生策略 180、CLI 配置/迁移 225）；build/baseline-dev8-m2-20261010/validation/x64-Debug-m1.json |
| Release M1 | passed，405 项（原生策略 180、CLI 配置/迁移 225）；build/baseline-dev8-m2-20261010/validation/x64-Release-m1.json |
| Debug 既有核心回归 | passed，2406 项；build/baseline-dev8-m2-20261010/validation/x64-Debug-core.json |
| Release 既有核心回归 | passed，2424 项；build/baseline-dev8-m2-20261010/validation/x64-Release-core.json |

M1 验证包括列表/读取/搜索/图像共同审批、目录外及只读拒绝、跨项目同名 workspace 绑定、策略版本、任务归属、工具关闭、身份上限和后端不可用。配置专项对六种 execution 来源（controlled/user/administrator × 开/关）实际迁移并读取新配置再运行策略判断，校验原字节备份、原配置/旧 Data 保留、重复输出/备份冲突拒绝及兼容字段不能扩权。原始专项证据分别在 tests/test-results/20261010-081917-m1-2d3fa371 和 20261010-081944-m1-d208f4bd，首轮通过的较早候选报告另存 build/validation/m1-initial-pass-20261010，不能代替最终摘要匹配报告。

核心回归仍启动真实 stdio、回环 HTTP、普通任务和既有本机审批流程；两轮明确排除历史 Connection: close 诊断，管理员 UAC 专项和开发机不支持的硬链接夹具为 skipped。M1 本轮不需要修改管理员执行实现，没有请求 UAC；不把新策略单元验证当作实际多项目路由、管理员共享代理或客户端运行验收。历史 HTTP close 问题仍待调查，Server 2025 dev.8 和真实 MCP 客户端未验收。

最终 Release 为 631808 字节，SHA-256 `9891cdae3593fcf2fc9d94b9cec5e9f827ababb0c0d93463c7ce4277198fcc4a`；Debug 为 2486784 字节，SHA-256 `dbec7bb483b3db0a4a6a7eafaf2cede1fa10ef8ab1d81a8fc936b64cb71a6d0c`。保留的 dev.7 Release 仍为 `7c2db66cad9f023f51b5b5bc0c7f4ed51cfe223558e2023dfba8105e7a3198dd`，旧最终 ZIP 仍为 `10b7d46535189a066f46428e964be782d4adbf5a82772e235bf9195a384a945f`。

首次沙箱内链接出现 MSPDB140.DLL 版本错误，沙箱内 Python 读取新程序真实路径也出现 WinError 5；同一 Visual Studio/MSVC 安装在工具审批后的沙箱外构建和测试均通过，没有修改系统工具链或安全设置。中途诊断构建日志保存在 build/m1-debug-build.log；不把这种环境失败记录成产品功能失败，也不删除原始证据。

**M1 已完成配置、策略和迁移基础；M2–M7 未完成。**下一项是普通权限管理器与每项目工作进程。M4 才接通新版完整文件/命令审批、批准记录消费、分页预算及受限执行；M5 才提供可信项目管理员代理；M6 才提供远程审批和持久任务。当前统一 evaluator 不接收自报批准结果，也不执行操作。正常新格式只做路径语法检查，运行层仍须真实句柄核验及管理资源保护；新配置不能作为未完成沙箱的启用开关。

本轮仍在 E:\Program\ShellBot 的 main 上，HEAD 不变，未提交、未推送、未发布。程序位置为 out/x64/Release/WinShellBot.exe；没有切换用户的实际运行配置或 Data。现有 package-test.ps1 的 dev.7 原生专项摘要门槛会拒绝新程序，M1 不降低该门槛、不冒用旧管理员/UAC 证据生成包。

## 已实现

通用 C17 核心包含配置、JSON-RPC、协议适配、工具注册、Unicode/base64 和审批状态机，不包含 Windows 头文件。Windows 层包含宽字符路径、实际句柄、目录范围、文件创建、系统信息、实例锁、系统随机数、SHA-256、同步锁和本地所有者管理通道。

现有十三个工具：host_info、host_capabilities、fs_list、fs_read、fs_search、image_view、fs_write、fs_patch，以及 task_start、task_status、task_output、task_input、task_cancel。内容搜索支持 UTF-8/UTF-16；完整 fs_read 返回原始字节摘要；覆盖和局部替换绑定原始哈希、本机审批和私有备份，见 FILE_EDIT.md。图像仍只检查格式头和预算，完整解码、缩放和文档后端待实现。

三个 2025 年 MCP 版本使用 initialize；2026-07-28 使用每请求元数据、server/discover、工具调用、resultType、服务信息及缓存范围。HTTP 验证 protocol/method/name 请求头和请求体一致性，支持规范中的 base64 头部哨兵，拒绝关键字段重复；stdio 不混用两种协议方式。没有 SSE、标准 MCP tasks 扩展或客户端 elicitation，不把本地审批伪装成已实现的 MCP 交互能力。

授权目录最多 16 个，没有隐含的 cwd 授权。根目录默认只读，配置 read_only:false 仅允许提出写入请求，仍需本机审批。读取检查句柄最终路径；创建验证并固定父目录句柄，通过 NtCreateFile 相对创建临时文件，写入、刷新后通过 NtSetInformationFile 禁止覆盖地提交。目标已存在或执行期间出现同名文件时，不替换它。审批参数包含目标及父目录的卷标识、128 位文件标识、UTF-8 内容 SHA-256 和字节数。

所有者可在本机查看完整内容、批准、拒绝、撤销；默认请求创建后 300 秒过期，配置范围 1～600 秒，队列最多 64 条。批准状态原子转为 running，完成或失败后可查询既有记录，不重复创建。许可记录只保存在内存，重启全部失效。dev.3 把审批与文件创建事件持久保存为受保护的 JSONL，日志跨重启保留，日志失败时阻止新批准和执行，见 [审计行为](AUDIT.md)。崩溃可能留下本次创建的临时文件，不按文件名前缀自动删除，没有重启任务恢复。

管理通道是拒绝远程客户端的 Windows 命名管道，使用显式 ACL，并模拟客户端校验实际账户 SID。dev.6 进一步拒绝限制 SID、低完整性和 AppContainer 调用者；HTTP 的 --stop 也通过该通道完成，stdio 关闭输入流停止。同账户的普通本地程序仍属于信任范围，不宣称全面区分人工操作和自动程序。三档配置和身份基础层进度见文末；完整受控隔离、规则和独立远程审批仍未实现；普通/管理员任务已实现，具体设计见 [三档执行权限](EXECUTION_PERMISSIONS.md)。

HTTP 仅监听 127.0.0.1，要求 bearer，拒绝格式错误的 Host/Origin、URL 凭据和重复安全头。没有自动静态文件服务、Cookie 认证或 CORS。旧单实例一个调用身份，项目网关每项目一个 bearer 身份，clientInfo 不是授权身份。dev.5 默认由已有代理决定域名，public_url 作为可选限制保留；用户明确不要求域名检查，OAuth 和 cloudflared 管理暂缓。

## 本机验证

开发系统为 Windows 11 专业工作站版，NT 10.0.26300。工具链为 VS 2026 Community v145、Windows SDK 10.0.26100.0。自有 C 代码 /std:c17、/W4 /WX；第三方固定版本源文件未修改，独立关闭警告；Release 静态链接 MSVC 运行库。

先前 dev.2 的 Debug 与 Release x64 核心套件各通过 521 项断言。测试启动真实 stdio 和 HTTP 进程，包含 Unicode 分页、图像字节、多根目录、目录联接逃逸、创建请求无副作用、查看完整内容、批准/拒绝/撤销/过期/重启失效、队列预算、目录实体替换、禁止覆盖、参数篡改、16 次并发写入重试、100 次并发 HTTP 查询、持续连接和本机停止。

当前 dev.11 核心报告为 build/validation/x64-Debug-core.json 与 x64-Release-core.json；dev.10 原报告另存 build/baseline-dev10-m4-20261010/validation，dev.9 在 build/baseline-dev9-m3-20261010/validation，dev.7 在 build/baseline-dev7-m1-20261010/validation。核心范围明确排除关闭连接诊断，并记录未执行的夹具和管理员启动。旧基线与 ZIP 保留。

历史完整测试包含 100 次客户端主动关闭连接的诊断；开发机曾出现 WinError 10054 或响应提前结束，因此完整套件未通过。独立 Python 参考 HTTP 服务在当前机器同样复现：100 次 keep-alive 调用无失败，100 次 close 调用有 13 次失败。Windows PowerShell 原生 TCP 测试也复现，不是 Python 客户端专有现象。已尝试显式有界 linger、禁用 Nagle 和合并响应发送，不能据此声称根因已解决。网络配置、操作系统和实现的具体责任仍待进一步证据。

旧 dev.2 Release 完整报告保存在 build/validation/x64-Release-full.json，关闭连接诊断为 100 次中失败 14 次，不通过重试隐藏失败。该文件不代表当前 dev.11 的报告，需按 artifact.sha256 区分。独立参考诊断为 build/http_reference_probe.py；测试报告、缓存和产物不入版本控制。dev.2 的直接 DLL 依赖为 WS2_32、bcrypt、SHELL32、ole32、ADVAPI32、KERNEL32，没有额外 CRT DLL。

## Server 2025 测试交付

scripts/package-test.ps1 要求核心通过报告匹配当前 Release 文件，生成独立测试 ZIP。只复制明确列出的可执行文件、PowerShell 测试脚本、说明、配置样例、依赖许可及开发机验证记录；不复制 Data、凭据、缓存、PDB 或已有测试数据。包含 manifest.json、来源文件摘要及 SHA256SUMS.json，标注本地源码有未提交修改。

tests/Test-ShellBot.ps1 已用 Windows PowerShell 5.1 实际运行，核心检查通过，关闭连接诊断保持单独计数。它不需要 Python/Node/Visual Studio，不修改系统配置、不安装服务、不开放外网；仅操作新建隔离目录，正常完成后停止自己启动的主机。报告及测试文件保留，便于回传。详见 [Server 2025 测试步骤](SERVER_2025_TEST.md)。

2026 年 10 月 6 日，用户在 Windows Server 2025 上回传控制台结果：Core: passed；Connection-close diagnostic: 2/100 failed。使用的包目录为 E:\Program Files\ShellBot-0.1.0-dev.2-windows-x64-testing-20261004-094828，报告目录为 test-results/20261006-145240-8521abfb。这是独立机器上的核心脚本通过反馈，完整网络验收仍未通过；尚未读取该机 report.json，具体系统 build、文件 SHA-256 和 Server Core/桌面体验类型未核实。

同日用户回传复测结果：原 Test-ShellBot.ps1 核心通过、关闭连接 0/100 失败，报告为 test-results/20261006-150813-bb89ed55；Test-ShellBot-Http.ps1 -HttpOnly 整体 passed、Core:not_run、Basic HTTP:passed，close 与 fresh keep-alive 各 0/100 失败，报告为 test-results/20261006-150910-9ba8a44c。Core:not_run 是按选项跳过核心，不是功能失败。此次 dev.2 网络诊断通过；结合此前偶发失败及开发机复现，不能称根因已修复，也不扩展为公网或后续 dev.3 验收。完整报告尚未读取。

本轮保留现有 dev.2 可执行文件及既有 ZIP，新增 HTTP-only 诊断和匹配的 fresh keep-alive 对照。Windows PowerShell 5.1 在开发机对现有 dev.2 的实测：持续连接检查通过，fresh keep-alive 对照 0/100 失败，close 组 7/100 失败；七次失败均在读取响应头前，header bytes 为 0，Windows 错误码 10054。此结果缩小了失败阶段，没有证明根因或修复。

独立参考服务的额外试验使用标准关闭和发送半关闭后读取至对端结束两种方式，每组 200 次，分别失败 52 次、4 次。两组都未达到零失败，不把这个差异视为已确认的因果关系；CivetWeb 已有关闭前读取逻辑，不能仅凭修改延迟声称修复。依据：[HTTP 连接收尾](https://www.rfc-editor.org/rfc/rfc9112.html#section-9.6)、[微软 Winsock 关闭说明](https://learn.microsoft.com/en-us/windows/win32/winsock/graceful-shutdown-linger-options-and-socket-closure-2)。

新的报告 schema_version 为 2，记录失败阶段、字节数、耗时和 socket 错误码，明确区分计划/完成调用及基本 HTTP/整体诊断状态。失败记录不包含原始头、响应正文、令牌或任意异常消息。新增脚本无需替换已有程序，使用方法见 [HTTP 对照诊断](HTTP_DIAGNOSTICS.md)。

目标 Win10/11 正式环境及真实模型客户端仍待独立验收。接口字节检查不代表 GPT 已实际看到图片，HTTP 本机检查不代表公网授权链路可用。

## dev.3 审计实现与验证

core/policy/audit.c 序列化固定元数据，由 Windows 句柄后端创建私有文件、写入并刷新。每实例新建日志，序号在互斥锁内分配。策略锁只保证状态消费，日志锁保证并发追加；不存在从审计层反向取得策略锁的路径。过期、拒绝和撤销仍会限制许可，日志故障不会重新授予许可。

审计范围包括审批状态与 create-only 创建，不宣称已记录全部读取、拒绝请求、HTTP 调用或后台任务。正文和令牌不进入日志；旧日志不恢复旧审批。若文件已提交而完成事件写入失败，仍返回 completed 与 audit_warning，重试查询原结果。日志预算默认每实例 8 MiB，不自动归档或删除；具体行为见 AUDIT.md。

Debug 与 Release x64 的新核心套件各通过 703 项断言，增加真实日志预算耗尽、失败时批准和执行阻止、失败时拒绝/撤销、已有完成结果查询、仅记录意图后结果写入失败、旧日志与崩溃尾部保留、并发执行事件去重、内容脱敏和时钟检查。Release 大小 492544 字节，SHA-256 为 e2862a435d699a7c93f5b9f4cee8f16cf2b770ecb3cf0c5992d33f56e0d94516。验证范围明确排除已知的连接关闭诊断。

dev.3 的 Windows PowerShell 5.1 全部核心及基本 HTTP 检查通过，包含实际文件 ACL 验证；开发机 close 12/100 失败，fresh keep-alive 0/100 失败，整体 needs_investigation。报告为 tests/test-results/20261006-151811-f18f15a1/report.json。此版没有改动 HTTP 发送与连接收尾逻辑，不把审计改动称为网络修复。

## dev.3 Server 2025 连续两次独立测试

2026 年 10 月 6 日已读取用户提供的两份完整 report.json，确认系统为 Microsoft Windows Server 2025 Datacenter、10.0.26100、build 26100。两份程序版本均为 ShellBot 0.1.0-dev.3 (C17)，SHA-256 均为 e2862a435d699a7c93f5b9f4cee8f16cf2b770ecb3cf0c5992d33f56e0d94516，与当前 Release、核心验证报告及交付包完全一致。报告未包含 Server Core/桌面体验类型，不从系统名称推断这一项。

| 运行（北京时间） | 报告编号 | 核心 / 基本 HTTP | 通过检查数 | close | fresh keep-alive | 整体 |
| --- | --- | --- | --- | --- | --- | --- |
| 15:27:09 | 20261006-152709-e9b15cee | passed / passed | 152 | 1/100 失败 | 0/100 失败 | needs_investigation |
| 15:27:22 | 20261006-152722-012ef045 | passed / passed | 152 | 0/100 失败 | 0/100 失败 | passed |

两轮均实际完成每组 100 次探测，未隐藏失败。新增审计检查包含审批/写入事件只记录一次、内容脱敏、顺序和版本、私有 ACL 限制为当前账户与 SYSTEM、重启保留旧日志及创建独立新日志、旧审批重启失效。Server 2025 的基础运行与上述审计场景可标记通过；日志预算和结果记录失败的专项验证仍是开发机证据，不扩大为已在 Server 上执行。

第一轮唯一失败为 close 组零起始索引 42，阶段 response_headers，响应头和正文均收到 0 字节，耗时 15 ms，SocketError:ConnectionReset、native_error_code:10054。第二轮没有失败。因此在 Server 2025 上也证实了相同失败阶段和错误码，合计 close 1/200 失败、fresh keep-alive 0/200 失败；相同错误表现不证明相同根因，仍不能认定 TCP、操作系统、网络过滤组件或 HTTP 实现的责任。

用户指定的共享目录只用于读取两份报告，未运行或修改测试机程序。原始字节及其完整检查项归档在 build/validation/server2025-dev3/20261006-152709-e9b15cee-report.json 与 20261006-152722-012ef045-report.json，复制前后校验一致；这些运行记录不进入版本控制。没有重新编译或替换 dev.3 发行包。

## 剩余工作

当前下一项为 M3 项目 HTTP 路由与认证，随后按主计划继续受限执行和管理权限隔离、独立远程审批、审批/任务重启恢复及完整审计、实际客户端图像和工具使用、目标系统独立验证、连接关闭根因和修复及正式发行版权主体。普通用户和管理员后台任务、输入、取消与进程树清理已有 dev.7 本机证据，dev.9 本轮回归普通任务与项目生命周期；管理员新版本与 Server 2025 命令回归仍待独立验收。覆盖和局部替换已实现，原子保存及故障恢复验收仍待后续；OAuth、隧道管理与域名检查按用户要求暂缓。

## dev.4 外部地址与内容搜索

用户要求假定已有隧道和域名，不开发 OAuth、公网配对或 cloudflared 管理，已同步到阶段计划与架构范围。http.public_url 或 --public-url 接受域名、http(s) origin 及端口，规范大小写与默认端口，不接收凭据或路径；Host/Origin 必须属于回环地址或明确配置值，不从转发头自动授权。能力查询返回 public_endpoint 与 tunnel_management:false，oauth 仍为 false；本机 bearer 和审批继续使用。

core/tools/search.c 接管有界搜索；filename 默认行为保留，content 支持 UTF-8、UTF-16 LE/BE、中文和 emoji，按命中行返回片段和 UTF-8 字节列号。单文件、累计字节、条目、层数、时间及返回量均受限制，越界路径拒绝，重解析目录跳过，二进制/无效编码统计跳过，不建立索引或修改文件。详见 EXTERNAL_ACCESS.md 与 CONTENT_SEARCH.md。

Debug 和 Release x64 核心套件各通过 870 项断言，包含地址规范化、非法域名/凭据拒绝、外部 Host/Origin、匿名调用拒绝、转发头伪造、UTF-8/UTF-16 命中、LF/CRLF/CR 行号、长行 Unicode 片段、单文件/总读取量限制、二进制跳过与目录联接越界。Release 大小 503296 字节，SHA-256 为 da8cbf220835bbb96d8192d62b2da4888aa4e1c79775b782dcb130cf57644cdf。核心范围仍明确排除关闭连接诊断。

Windows PowerShell 5.1 脚本的核心与基本 HTTP 检查通过，close 9/100 失败、fresh keep-alive 0/100 失败，整体 needs_investigation；报告位于 tests/test-results/20261006-155722-f22e7c5d/report.json。未把域名配置描述为连接重置修复；此时未实际访问任何用户外部域名，Server 2025 验证结果见下一节。

已从 dev.4 ZIP 解压到独立目录，逐项核对 12 个载荷文件的 SHA-256，并直接使用包内程序和脚本在 Windows PowerShell 5.1 运行：160 项核心与基本 HTTP 检查通过，close 1/10 失败、fresh keep-alive 0/10 失败。报告位于 build/package-verification-dev4-20261006-080021/ShellBot-0.1.0-dev.4-windows-x64-testing-20261006-080021/test-results/20261006-160257-9fb590dd/report.json。随后仅同步计划和进度文档重新打包，程序与测试脚本字节不变；不把这次包内验证扩大为独立测试机或公网验收。

## dev.4 Server 2025 独立测试

已读取用户提供的 20261006-161731-6534f05c/report.json，测试于 2026 年 10 月 6 日北京时间 16:17:31 至 16:17:37 进行。系统为 Microsoft Windows Server 2025 Datacenter、10.0.26100、build 26100；程序 SHA-256 为 da8cbf220835bbb96d8192d62b2da4888aa4e1c79775b782dcb130cf57644cdf，与 dev.4 交付包和本地 Release 相同。

报告 status、core_status 与 http_basic_status 均为 passed，160 项检查全部通过；close 和 fresh keep-alive 各完成 100 次，均为 0 次失败。新增 UTF-8/UTF-16 内容搜索、行号和 Unicode 片段、结果数量限制、默认文件名搜索、外部 Host/Origin 接受与未知域名拒绝均通过。既有本机审批、创建、审计 ACL 和重启保留场景也通过。该轮运行未复现连接重置，不推断其根因已经修复。

原始报告以字节复制到 build/validation/server2025-dev4/20261006-161731-6534f05c-report.json，复制前后 SHA-256 均为 be98405299f5d638014a832d0d571beff5dc9adb8a4b56c624f66f4ec92f7616；未运行或修改共享目录中的测试机程序。报告使用本机临时端口及示例域名请求头，不包含实际隧道或模型客户端调用。

用户随后提供 shellbot.jsnowly.win 和现有 cloudflared 隧道，表示源站配置为 https://localhost:8765。从开发机对 https://shellbot.jsnowly.win/mcp 的无令牌请求收到 HTTP 502。ShellBot 8765 是纯 HTTP、IPv4 回环监听，已告知源站应为 http://127.0.0.1:8765，外部域名继续使用 HTTPS，且需要保持主机运行；未更改隧道或测试机配置。502 只作为当前可观察结果，不单凭它认定全部根因。

新增独立 Test-ShellBot-Endpoint.ps1，通过 Windows PowerShell 5.1 语法解析和原生主机集成验证：认证链 20 项检查通过；缺少令牌时仅做匿名拒绝，返回 needs_token；错误令牌在认证健康检查被 401 拒绝；报告未包含测试令牌。文件位于 build/endpoint-probe-validation-20261006-162509。以不设置令牌的方式实际访问用户域名，匿名阶段记录 HTTP 502，报告为 build/validation/public-endpoint-dev4/20261006-162635-cadcae3d/report.json。该轮未改动可执行文件或交付的 dev.4 ZIP。之后用户明确不要求域名检查，此脚本保留为可选工具，不进入当前常规测试包或开发前置条件。

## dev.5 本机端口、文本覆盖与局部修改

按用户最新要求，不再检测真实域名；默认监听 127.0.0.1:8765，已有外部代理负责域名。未填写 public_url 时接受合法的代理 Host/Origin，继续要求 bearer、拒绝重复头和 URL 凭据，不使用 Cookie 认证或 CORS；填写 public_url 后继续执行明确白名单。核心测试只在回环地址使用示例请求头，不进行 DNS 或公网调用。

完整 fs_read 返回原始字节 SHA-256。fs_write 默认创建不变，覆盖要求 overwrite:true 和 expected_sha256；新增 fs_patch 要求原始摘要、非空 search、replacement 和精确非重叠匹配次数。审批查看可检查原文和最终文本，许可绑定全部参数、调用者、父目录及文件身份。执行期间重新核对原文，拒绝内容或对象替换冲突；结果与失败重试复用既有状态。

修改采用原文件独占句柄，先在私有 Data 刷新完整原字节备份并记录 backup_saved，再写入、截断和刷新。保留文件身份与 ACL，不承诺崩溃原子性；普通 I/O 失败尝试回滚，突然中断需所有者用私有备份恢复，相关故障恢复分支尚未注入验收。原文和结果均限 64 KiB UTF-8，每实例最多 64 份备份，保留不自动删除；UTF-16、二进制、重解析文件、多硬链接、只读或被占用文件拒绝。完整行为见 FILE_EDIT.md。

Debug 和 Release x64 各通过 1515 项已执行核心检查。包含匹配次数和字节上限、BOM/换行、完整摘要、原文和文件身份冲突、缓存重试、备份字节、64 份备份预算、backup_saved 审计失败时不改原文，以及无需域名的代理调用和认证拒绝。E: 为 ReFS，无法创建硬链接夹具，该项明确列为 skipped，NTFS 多链接拒绝仍需独立验证。

Windows PowerShell 5.1 脚本通过 195 项核心与基本 HTTP 检查，包括新备份 ACL 与代理接入；报告为 tests/test-results/20261006-170755-580252db/report.json。关闭连接 9/100 失败，fresh keep-alive 0/100，整体 needs_investigation。此前 20261006-170158-5883112e 的脚本把不受支持的空 CLI 参数用于代理测试导致启动失败，已改为独立无 public_url 的配置并验证；也修正 HTTP 阶段出错时覆盖既有核心通过状态的报告逻辑。没有把新增功能称为连接重置修复。随后收到的独立机器结果见下一节；旧版本归档、报告和 ZIP 均保留，main 未提交或推送。

## dev.5 Server 2025 独立测试

已读取用户提供的 20261007-125434-ec039ee1/report.json，测试于 2026 年 10 月 7 日北京时间 12:54:34 至 12:54:44 进行。系统为 Microsoft Windows Server 2025 Datacenter、10.0.26100、build 26100；程序版本为 ShellBot 0.1.0-dev.5 (C17)，SHA-256 为 6e3bf2daa3a3c551286793403bffa807414254250441531272ce0b3f00b00e8e，与本地 Release、核心验证记录及已交付 dev.5 ZIP 的 manifest 相同。报告未说明 Server Core 或桌面体验类型。

| 验证项 | 结果 |
| --- | --- |
| 核心 / 基本 HTTP | passed / passed，195 项检查全部通过 |
| Connection: close | 完成 100 次，1 次失败 |
| fresh keep-alive 对照 | 完成 100 次，0 次失败 |
| 整体状态 | needs_investigation |

新增完整读取摘要、审批查看原文与新文本、绑定原始哈希的覆盖和局部替换、准确截断、原文备份字节、备份私有 ACL、摘要冲突及审批后内容变更拒绝、完成结果重试均通过。既有创建、审计、重启失效与保留场景，以及不填写 public_url 的本机代理请求头和 bearer 检查也通过。上述 dev.5 基础修改能力已有 Server 2025 独立机器证据；多硬链接拒绝、备份与审计预算耗尽、强制 I/O 失败回滚及崩溃恢复不属于这份报告的验收范围。

唯一连接失败发生在 close 组零起始索引 59（第 60 次），阶段 response_headers；响应头及正文均收到 0 字节，耗时 26 ms，SocketError:ConnectionReset、native_error_code:10054。既有偶发关闭问题仍未修复；不因错误码相同认定根因，也不把此前 dev.4 的零失败或这一轮失败解释为修复或回归的证明。

原始报告以字节复制到 build/validation/server2025-dev5/20261007-125434-ec039ee1-report.json，复制前后远端与本地 SHA-256 均为 797dad206010cb08eff8cd3e87246f030bb9da94eefc54d94b325fe3b0884bf9。仅从共享目录读取报告，未运行或修改测试机程序。已交付 ZIP 保持不变；本次只归档证据和更新项目文档，未重新构建、提交或推送。

报告采用回环地址及示例代理请求头，不包含实际隧道或模型客户端调用；真实域名检查继续按用户要求排除。下一步仍是先隔离执行身份与本机管理权限，再实现受控命令和后台任务；Win10/11 独立验收、实际客户端图像与工具使用及连接问题继续保留待完成状态。

## dev.6 三档权限与身份基础层

按已确认计划先实现权限基础层，仍保持八个既有工具。execution 配置新增 enabled、profile 与受控档 approval_mode；默认关闭并选择 controlled。严格校验未知字段、重复键、类型、档位与审批组合，不接受自报 approved/elevated，也尚不支持规则列表。host_info、host_capabilities 和本机 --execution-status 返回配置意图与真实进程令牌，区分普通身份、已提升身份、限制 SID、完整性级别及 AppContainer，避免仅按配置字符串报告权限。

Windows 执行状态独立于通用核心保存原生句柄。controlled 可准备低完整性受限令牌，所有者管理检查在 SID 相同之外拒绝限制 SID、低完整性和 AppContainer；开发机实测受限调用者无法使用运行中主机的管理入口，普通所有者仍能使用。该证据不扩大为完整文件、网络、桌面或子进程沙箱，controlled_isolation_verified 保持 false。user 初始化要求真实普通令牌，已提升主机选择此档会报错，不悄悄继承管理员权限。

administrator 显式启用后，通过 runas 在启动时请求常驻代理。代理只接受身份查询和退出消息，不接受命令。通道包含随机实例标识、受保护 ACL、远程管道拒绝及双向进程身份检查；主机校验启动返回句柄、实际管道对端令牌和身份回复，代理校验父进程与同一程序路径。客户端 SECURITY_IDENTIFICATION 阻止普通主机借管道模拟管理员。启动失败、取消或身份不匹配返回失败，不自动退回其他档位；代理死亡不会自动再次弹 UAC。主机退出关闭通道，代理在断开后退出；同账户实际提权、重复查询与退出清理已于 2026 年 10 月 9 日通过 Server 2025 独立验收，用户确认仅启动代理时一次 UAC；跨账户身份仍未覆盖，证据见下节。

Debug 与 Release x64 自有代码 /std:c17、/W4 /WX 构建通过，各完成 1582 项核心检查，验证三档配置、真实普通身份、受限管理调用拒绝、普通所有者继续可用、MCP 不能改档，以及未提升进程不能充当管理员代理。管理员启动/UAC 明确列为 skipped；开发机 E: ReFS 的硬链接夹具仍为 skipped。Release 大小 540160 字节，SHA-256 为 067a5acd24d0839adf4293803b95372ff5f33b8a49eb26abe533258b077cee26。

Windows PowerShell 5.1 常规脚本完成 199 项核心与基本 HTTP 检查，关闭连接 9/100 失败、fresh keep-alive 0/100，整体 needs_investigation；报告为 tests/test-results/20261007-150438-37bc2f41/report.json。权限专项脚本在普通权限下完成 36 项检查，管理员交互验收明确跳过，报告为 tests/test-results/20261007-150438-permissions-feb7c400/report.json。两个报告均匹配当前 Release，不把身份准备或构建成功当作管理员代理运行验收。

新增 Test-ShellBot-Permissions.ps1，可在独立电脑通过 -Administrator 启动代理、连续查询同一进程 20 次并检查退出。脚本报告真实提升及跳过项，不自动观察 UAC 弹窗次数、不测试任意命令。专项脚本和 EXECUTION_PERMISSIONS.md 纳入固定测试包清单。完整受控隔离、规则匹配、命令与后台任务、远程审批继续待实现；既有连接问题仍未修复，域名检查、OAuth 与隧道管理继续暂缓。main 未提交或推送。

## dev.6 Server 2025 权限基础层独立验收

2026 年 10 月 9 日已读取并核验用户提供的两份原始 report.json。系统均为 Microsoft Windows Server 2025 Datacenter、10.0.26100、build 26100。程序 SHA-256 为 067a5acd24d0839adf4293803b95372ff5f33b8a49eb26abe533258b077cee26，与当前 Release 及已交付 dev.6 测试包 manifest 一致。包和可执行文件保持不变；本次仅补充验收记录，不重写旧包内的开发机报告或源码快照。

| 运行（北京时间） | 报告编号 | 结果 |
| --- | --- | --- |
| 21:22:19 至 21:22:29 | 20261009-212218-ab76d7a2 | 核心和基本 HTTP passed，199 项；close 1/100 失败，fresh keep-alive 0/100，整体 needs_investigation |
| 21:22:49 至 21:22:52 | 20261009-212249-permissions-11d7cd04 | 权限身份基础层 passed，100 项；普通权限主机、受限管理调用拒绝、实际提升后的管理员代理、20 次复用查询及主机退出后代理退出通过 |

权限报告 host_elevated:false、administrator_agent_identity_elevated:true、administrator_identity_matches_host:true。因此本次覆盖同账户提权，跨账户 UAC 身份为 skipped。脚本 uac_prompt_count 仍为 not_observed_automatically；用户在本次对话中明确确认“仅启动代理时出现一次”，该信息作为用户观察记录，不能改写成脚本自动测量结果。此证据验收身份代理的运行基础，不扩大为任意命令执行、完整受控隔离、规则匹配、远程审批或跨账户提权。

连接关闭唯一失败为第 68 次请求，在 response_headers 阶段发生 ConnectionReset / 10054，响应头及正文均为 0 字节，耗时 52 ms。保留偶发连接重置未修复状态，不通过重试隐藏失败。

原始报告按字节归档于 build/validation/server2025-dev6/20261009-212218-ab76d7a2-report.json 与 20261009-212249-permissions-11d7cd04-report.json，SHA-256 分别为 f7ce4cd5f414517db885125152e1b8ccf510d25ebdd16d43991c54bd13483bdf 和 6409d0b5fa543cc944079b2b759d91039ee7cbedac0bd78ab6a338d7337d37ca。acceptance.json 分别记录自动检查与用户观察，验证资料不入版本控制。

Windows 11 是主要开发和目标平台，可继续本机回归；Server 2025 作为独立服务器环境补充验证。可在用户配置 SSH 后远程运行非交互自动化并回收报告；应记录 SSH 会话的实际执行身份，不把 SSH 中已有管理员权限当作普通桌面首次 UAC 验收。UAC 和桌面操作仍需交互会话证据，SSH 配置不是继续开发的前置条件。

## dev.7 本机原生命令与后台任务

新增工具与限制见 COMMAND_TASKS.md。CreateProcessW 在挂起状态建立子进程，明确继承 stdin/stdout/stderr，完成 Job 归属后恢复；有限环境排除 bearer 及未知变量，后台读取保留原始字节。取消、期限和输出超限终止所属 Job；进程树仍活跃时不因根进程退出误报完成。

每个 task_* 请求绑定 host_instance_id；同实例 request_key 重试不重复启动，input_key 重试不重复追加。任务回执最多 32 条、活动任务 8 个，输出每流 256 KiB，输入 4 KiB/次、64 回执；不自动淘汰旧键，不提供重启恢复。目录用于 cwd 定位，任意命令保留选定账户权限。受控档仍明确不可用。

Debug/Release 构建使用 C17、/W4 /WX。核心套件覆盖真实普通任务、Unicode/引号/空参数、中文 cwd、二进制输出、真实进程树、无关进程保留、环境脱敏、超时/输出限额、内存回执上限、重启实例拒绝、HTTP 并发相同键只启动一次及任务审计预算故障。核心报告保留管理员交互 skipped，该路径另由原生专项记录；开发机 ReFS 硬链接夹具仍 skipped。

常规 PowerShell 5.1 在 tests/test-results/20261009-220941-79466777/report.json 通过 199 项核心/基本 HTTP，close 0/100、fresh keep-alive 0/100。本轮零失败不证明历史偶发重置已修复。默认权限专项在 20261009-221008-permissions-fcddfb0d/report.json 通过 36 项，管理员路径跳过。

最初任务专项使用 EncodedCommand，出现一次启动失败，随后同一方式通过 85 项；用户同时报告杀毒软件 Powershell Attack 告警，未确定单次失败根因。已删除该专项的编码 PowerShell 子命令，改为测试专用 C 原生辅助程序，并附源码。普通专项 20261009-222714-tasks-68850ebc/report.json 通过 74 项，管理员专项 20261009-222737-tasks-3e08e86d/report.json 通过 79 项；主入口未提升，任务实际令牌符合档位，同一管理员代理复用，退出清理通过。用户另行确认“没有新的杀毒告警，UAC 仅出现一次”；作为人工观察写入独立凭据，不改写脚本 not_observed_automatically 字段。未更改杀毒、UAC 或系统执行策略。

该轮为 Windows 11 专业工作站版 build 26300 本机验收，Server 2025 dev.7 命令、跨账户管理员、受控隔离、规则、远程审批和真实客户端工具使用仍待完成。任务审计仅记录操作意图及新启动结果，没有完整自然结束/输出消费事件链。

保留 build/baseline-dev6-tasks-20261009 和旧 dev.6 ZIP。核对 21 份已归档第三方源文件后删除冗余 build/downloads：2243 文件、69737801 字节；原始报告、源码、旧包和基线均保留。main 上仍为本地未提交、未推送实现。

管理员通信最后补充了转义输出边界修复：16 KiB 合法 UTF-8 控制字节可能生成超过 64 KiB 的 JSON 返回；请求仍限 64 KiB，管理员返回限 256 KiB。原生专项新增最大控制字节输出，核对 base64、UTF-8 文本以及返回后代理继续存活和复用。修复前候选 ZIP 保留并标记 superseded；首个解压检查器未找到报告，随后直接运行相同脚本任务通过、常规核心和基本 HTTP 通过，close 1/100、对照 0/100。保留检查器失败及原始报告，不确定其首次启动原因，不把诊断失败隐藏或归因于杀毒。

最终 Debug 核心通过 2376 项，Release 核心通过 2424 项；轮询次数随实际完成时间变化。普通原生专项 tests/test-results/20261009-224843-tasks-917dca8c/report.json 通过 89 项，管理员原生专项 tests/test-results/20261009-224928-tasks-77881387/report.json 通过 94 项。最终常规 tests/test-results/20261009-225020-20943e19/report.json 核心/基本 HTTP 199 项通过，两组诊断均 0/100；历史偶发重置仍未修复。Release 586240 字节，SHA-256 7c2db66cad9f023f51b5b5bc0c7f4ed51cfe223558e2023dfba8105e7a3198dd。本机凭据汇总 build/validation/dev7-local-acceptance.json 将人工观察限定到首次原生专项，不把弹窗计数称为自动统计；最终管理员运行仍由脚本证明真实权限、同代理复用和退出清理。


## 2026 年 10 月 9 日开发计划调整（仅文档）

用户已确认：多项目分别使用 MCP 链接，每项目多个工作目录；always、workspace_auto、never 审批与文件范围和执行身份分开；目录外默认拒绝；允许同时配置完整权限项目和目录内自动读写、命令审批的项目。普通权限管理器维护独立工作进程，管理员代理按项目授权共享，一个项目获准不授予其他项目。共享代理已提升后新增项目需要所有者授权，不承诺 Windows 再次弹 UAC。

新增 DEVELOPMENT_PLAN.md 作为当前迭代主计划，同步 ARCHITECTURE.md、PHASE_1_PLAN.md、EXECUTION_PERMISSIONS.md 和 README.md，保留旧 dev.7 配置与测试证据。下一项代码工作为 M1 项目配置与统一策略，然后按 M2–M7 推进工作进程、独立地址、完整审批和受限执行、共享管理员代理、远程审批与可靠性、最小界面及实际客户端验收。

本次不修改 C 源码、实际配置、运行数据、审批、凭据或测试包，不执行管理员专项，不声称新能力已开发。dev.7 程序版本和已记录验收范围不变；没有提交或推送。OAuth、cloudflared 管理和域名检查继续暂缓，历史 HTTP close 偶发重置仍待调查。
