# ShellBot 使用说明

适用版本：`0.1.0-dev.18` 开发源码及 `dev.14` 冻结候选包，Windows x64。更新日期：2026 年 10 月 12 日。

ShellBot 在受控电脑上提供 MCP 工具，供支持相应协议的模型客户端调用。当前可使用多项目、目录与文件操作、后台命令、所有者审批、共享管理员代理和本机管理窗口。电脑上不需要安装 Python、Node.js 或 ChatGPT 桌面客户端。

M1–M7 的本机开发与候选验证已完成。dev.15 新增独立授权的 [Windows 桌面工具](DESKTOP_CONTROL.md)，默认关闭；dev.16 增加图像解码与缩放，dev.17 增加 [PDF / Office 阅读与预览](DOCUMENT_READING.md)，dev.18 增加 [产物上传与下载](ARTIFACT_TRANSFER.md)。原 dev.14 候选包不包含 M8–M11。管理员传输专项待用户方便时配合。独立 Windows 10/Server 2025、另一设备及已有代理、实际 MCP 客户端验收仍待完成；历史 HTTP 关闭连接偶发重置仍在调查。OCR、服务启动及跨平台功能属于后续计划。详见 [开发计划](DEVELOPMENT_PLAN.md)。

## 1. 准备程序和目录

本地候选包位于仓库的 `dist/ShellBot-0.1.0-dev.14-windows-x64-candidate-20261011-090856.zip`。解压到自己的程序目录，例如 `D:\ShellBot`。保留同目录的 `WinShellBot.exe` 与 `ShellBot-SandboxProbe.exe`；后者用于受限任务后端的启动检查。

源码构建的 dev.18 还会生成旁置 pdfium.dll，PDF 读取/预览需要保留同架构的固定版本 DLL；不要只复制主程序。Office 功能不要求安装 Microsoft Office。当前未制作新的 dev.18 候选包。

准备三个相互分开的位置：

| 用途 | 示例 | 保存内容 |
| --- | --- | --- |
| 程序目录 | `D:\ShellBot` | 程序、说明、实际配置 |
| 工作目录 | `D:\ShellBotWorkspace` | 允许模型访问的文件，必须预先存在 |
| 项目数据目录 | `D:\ShellBotProjects\example\state` | 私有审计、审批和任务历史、修改备份 |

项目数据目录不要放入授权工作目录；多个项目的数据目录不能相同或互相包含。管理器使用独立数据目录，包内启动脚本默认采用程序目录下的 `Data\Manager`。保留现有数据，首次试用使用新的目录。

复制包内 `manager.example.json` 为 `projects.local.json`，编辑以下两项为自己的绝对路径：

```json
"data_dir": "D:/ShellBotProjects/example/state",
"workspaces": [{ "id": "workspace", "path": "D:/ShellBotWorkspace", "read_only": true }]
```

这是配置片段，应修改原文件中的对应项，保留其余结构。示例项目 id 为 `example`；默认只读、每次文件访问审批，命令和管理员能力关闭。配置说明及双项目样例见 [项目配置](PROJECT_CONFIGURATION.md)。

在普通 PowerShell 中校验配置：

```powershell
Set-Location 'D:\ShellBot'
.\WinShellBot.exe --validate-projects .\projects.local.json
```

校验只检查配置，不启动项目。工作目录是否存在、实际权限和后端能力还会在项目启动时核验。

## 2. 启动管理器和窗口

在同一个普通 PowerShell 中分别生成项目凭据和所有者凭据：

```powershell
$env:SHELLBOT_EXAMPLE_TOKEN = (& .\WinShellBot.exe --generate-token)
$env:SHELLBOT_OWNER_TOKEN = (& .\WinShellBot.exe --generate-token)
.\Start-Manager.cmd
```

环境变量名必须与配置中的 `token_env` 和 `gateway.owner_token_env` 一致。每个启用项目的凭据值都必须不同，所有者凭据也独立。保管好这些值：项目凭据用于 MCP 客户端，所有者凭据用于审批页面。凭据不能写入 URL 或共享配置，也不要将所有者凭据交给模型。

上面的变量只设置在当前终端及其子进程中；管理器运行期间保持终端打开。以后重新生成凭据时，客户端也需更新。仅双击 `Start-Manager.cmd` 不会继承另一个终端临时设置的变量，首次使用按上述步骤启动。

出现 `manager_ready:true` 和 `http_gateway:true` 后，双击 `Open-Management.cmd` 打开管理窗口。选择 `example`，点击“启动项目”，再刷新并核对运行状态。注册项目不会自动启动。管理器和窗口都应使用同一账户的普通权限运行；需要管理员能力时再明确启动代理。

如果使用源码构建产物，或需要指定其他路径，可用以下命令替代包内脚本：

```powershell
$bot = 'E:\Program\ShellBot\out\x64\Release\WinShellBot.exe'
& $bot --manager --http --projects 'D:\ShellBotSettings\projects.json' --manager-data-dir 'D:\ShellBotProjects\manager\state'
# 在另一个普通终端执行以下命令，manager-data-dir 必须相同
& 'E:\Program\ShellBot\out\x64\Release\WinShellBot.exe' --manage --manager-data-dir 'D:\ShellBotProjects\manager\state'
```

使用自己的程序、配置和数据路径；启动前仍须在管理器所在终端提供配置所引用的凭据。

## 3. 连接 MCP 客户端

在管理窗口选择项目，复制本机 MCP 地址。默认示例项目为：

```text
http://127.0.0.1:8765/projects/example/mcp
```

在客户端新增支持 Streamable HTTP 的 MCP 连接，填写该地址并提供 `SHELLBOT_EXAMPLE_TOKEN` 对应的实际值。请求认证形式是 `Authorization: Bearer <项目凭据>`。客户端需要支持 ShellBot 的 JSON POST 响应及协议版本协商；具体填写位置由客户端决定，当前不支持 SSE 或标准 MCP tasks 扩展。真实客户端兼容性仍需单独验收。

项目地址和凭据必须配对。其他项目凭据或所有者凭据不能代替当前项目凭据。默认没有旧 `/mcp` 地址，只有显式配置 `gateway.default_project_id` 才提供该别名。

连接后先调用 `host_info`、`host_capabilities`，核对版本、项目、目录标识和实际能力。示例中的文件路径使用 `workspace_id: "workspace"` 和相对路径，例如 `path: "notes.txt"`。不需要模型自行猜测电脑上的绝对路径。

另一设备不能访问这里的 `127.0.0.1`。已有 HTTPS 代理需要转发到这台电脑的回环端口，再使用公共域名下相同的项目路径。可选 `gateway.public_url` 用于生成公共地址和限制请求域名；ShellBot 本身不安装隧道或提供公网 OAuth。详见 [项目网关](PROJECT_GATEWAY.md)及 [外部访问](EXTERNAL_ACCESS.md)。

只需要单实例只读文件访问、且客户端支持本机 MCP 子进程时，也可使用旧 stdio 入口：

```text
command: D:\ShellBot\WinShellBot.exe
args: --stdio --root D:\ShellBotWorkspace
```

这个入口使用只读目录 `workspace`，不通过多项目管理窗口运行；新项目配置应使用 `--manager --projects`，旧 `--config` 不接受新格式项目配置。

## 4. 文件、审批和命令权限

权限由文件范围、只读属性、审批模式、工具开关、执行身份和管理员许可共同决定。审批不会扩大目录范围，也不会将只读目录变成可写。

| 审批模式 | 当前行为 |
| --- | --- |
| `always` | 文件列表、读取、搜索、图像、写入和命令启动/输入均需批准；主机元数据查询无需逐次审批 |
| `workspace_auto` | 已授权工作目录内的文件操作自动执行；目录外已授权文件以及命令启动/输入仍需批准 |
| `never` | 已获配置许可的操作无需逐次批准；只读和身份上限仍生效 |

首次试用可保留 `always`。打开 `http://127.0.0.1:8765/owner`，输入独立所有者凭据，选择项目及普通/管理员后端，查看待审批记录。核对目标、完整参数和期限后批准或拒绝。仅打开页面或接受客户端提示不会批准。凭据只保存在页面内存中，关闭或清除后须重新输入。

工具返回 `pending`、`approval_id` 或 `approval_url` 时，完成审批后让客户端按原参数继续或重试，必要时携带对应 `approval_id`。新版客户端若收到 URL 提示，仍需所有者在页面实际批准。改变文件目标、内容或命令参数后需重新申请；过期、重启、暂停或撤销的许可不能继续使用。

当前文件工具还包括 `document_info`、`document_read`、`document_preview`。要允许写入，必须同时将相应目录设为 `read_only:false` 并在 `policy.tools` 中启用写入工具；示例未启用。覆盖或修改会核对原文摘要并保存私有备份，不能承诺崩溃原子性，详见 [文本修改](FILE_EDIT.md)。

源码 dev.18 的 `artifact_download` 用于传出完整文件，客户端先 open，经批准后沿 next_offset 读取 Base64 块并核对摘要。`artifact_upload` 用于传入新文件：open 时提供准确大小和 SHA-256，经批准后分块 write，最后 commit；不会覆盖已有文件或执行文件内容。单文件最大 64 MiB，每块最多 32 KiB。上传须显式启用工具和目录写权限；只读示例只启用下载。取消、过期或项目重启会丢弃未提交的暂存，重启不能续传。客户端需支持分块组装/发送；目前没有自动附件或匿名下载链接，具体参数见 [产物传输](ARTIFACT_TRANSFER.md)。

源码 dev.16 的 `image_view` 会完整解码 PNG/JPEG，并可加入 `max_dimension:1600` 保持比例缩小。源不超过 16 MiB 和 16,777,216 像素，返回图片不超过 2 MiB；较大的源自动缩小或转码，原文件不改动。实际尺寸与源/输出摘要随结果返回；衍生 PNG 不应用 EXIF 自动旋转，见 [图像阅读与缩放](IMAGE_VIEW.md)。冻结 dev.14 包保留旧的 2 MiB 文件头检查行为。

dev.17 文档阅读优先 PDF / DOCX / XLSX / PPTX，旧 DOC / XLS / PPT 应先转换。先用 document_info 查看单元数，再用 document_read 读取指定 unit、沿 next_offset 分页，或用 document_preview 查看 PNG。PDF 的 unit 是真实页；DOCX 只有一个正文单元，XLSX 按工作表、PPTX 按幻灯片。PDF 扫描图片不会自动识别文字；Excel 只返回已保存值，不计算公式。Office PNG 明确标注为内容预览，不保留原始页面排版、图表或图片。源最大 32 MiB，只读和批准要求与文件工具一致；详细示例见 [文档阅读](DOCUMENT_READING.md)。

要启用命令，需明确设置 `policy.execution.enabled:true`、选择身份并启用相关任务工具：

| 身份 | 配置和运行要求 |
| --- | --- |
| `restricted` | 默认身份；需显式声明 `runtime_files`，只能启动声明的程序，启动检查必须通过；限制网络、桌面调用及子进程，通用解释器或构建工具可能不能运行 |
| `current_user` | 普通账户执行；任意命令要求明确授予可写的当前账户文件范围，单靠工作目录不能构成目录隔离 |
| `administrator` | 每次操作明确选择，项目获准且共享管理员代理已启动；命令还需开启执行及工具，不因提权免除审批 |

修改配置后先保存旧配置，停止管理器，再使用新配置重启并显式启动项目。管理窗口当前只管理运行状态，不能直接编辑配置或在线重载。权限配置的完整约束见 [执行权限](EXECUTION_PERMISSIONS.md)和 [项目配置](PROJECT_CONFIGURATION.md)。

后台任务用 `task_start` 启动，保存返回的任务编号和请求键；用 `task_status` 查询状态，`task_output` 分页取输出，`task_input` 发送输入或 EOF，`task_cancel` 取消。查询已授权任务的状态、输出无需反复审批，输入另行审批。每个任务属于原项目及身份，管理员任务的后续操作也必须保留 `identity: "administrator"`。详见 [命令任务](COMMAND_TASKS.md)。

## 5. 管理员操作

默认示例关闭管理员能力。确有需要时按权限文档配置合适的项目，并明确授予管理员使用许可。受限或仅配置目录隔离的项目不能使用共享管理员代理。

在管理窗口点击“启动管理员”，确认后处理 Windows UAC。UAC 只在这次显式启动时请求；已获准项目可复用同一个代理。取消 UAC 后管理员能力保持不可用，原操作不会降为普通权限执行。启动代理不会自动给所有项目授权，配置许可和实际代理状态需分别核对。

停止代理会结束所有项目的管理员任务，普通任务继续运行；暂停或停止一个项目只结束该项目的相关任务。撤销项目管理员许可也会失效旧批准并取消该项目活动管理员任务，不能回滚已完成的文件或命令副作用。当前只接受同账户、同会话提权，跨账户提权不受支持。

运行期授权不会写回注册配置；管理器重启后按保存的配置恢复许可，代理仍需重新显式启动。详见 [共享管理员代理](SHARED_ADMINISTRATOR.md)。

## 6. 日常管理和退出

| 操作 | 结果 |
| --- | --- |
| 刷新、查看诊断 | 核对项目状态和实际后端能力，复制诊断需用户明确操作 |
| 关闭管理窗口 | 管理器和任务继续运行，可重新打开连接 |
| 停止项目 | 结束该项目普通及管理员任务，其他项目继续运行 |
| 暂停项目 | 撤离该项目后端并失效许可；需明确“恢复”，普通“启动”不能解除暂停 |
| 恢复项目 | 创建新实例并保留历史，旧任务、输入和批准不会自动重放 |
| 停止管理器 | 结束其管理的项目和管理员任务 |

项目暂停状态仅存在于当前管理器运行期。重启后项目均需显式启动。正常退出可在运行管理器的终端按 Ctrl+C；也可在另一普通终端执行：

```powershell
& 'D:\ShellBot\WinShellBot.exe' --manager-stop --manager-data-dir 'D:\ShellBot\Data\Manager'
```

这里的数据目录须与实际管理器一致。退出会结束活动任务，重启只读取历史并标记中断，不自动重跑。

## 7. 数据、升级和常见问题

源码构建的 dev.15 可按 [桌面工具说明](DESKTOP_CONTROL.md)配置观察、输入和剪贴板独立权限。使用前取得短期独占控制，动作后重新观察；人工输入会暂停，默认 Ctrl+Alt+F9 紧急停止，只能由本机所有者明确恢复。暂停恢复与项目暂停是不同入口；桌面权限覆盖当前会话内容，目录只读不限制它。完整只读示例见 [desktop.example.json](../config/desktop.example.json)。

实际配置、凭据、管理器和项目 Data 是自己的运行资料。升级前停止相关进程并备份；保留旧程序包及修改前配置。当前没有自动迁移、历史淘汰或升级回滚功能，不要用全新包覆盖或删除原 Data。

私有历史有大小、记录和实例数预算，达到上限时会拒绝新操作或启动；先停机审阅、归档，再准备新的数据位置。审批审计与文件备份也应保留，详情见 [审计](AUDIT.md)及 [历史与恢复](REMOTE_APPROVALS.md)。

| 现象 | 处理 |
| --- | --- |
| 管理器启动提示凭据缺失或重复 | 在管理器所在终端设置配置引用的环境变量；所有启用项目及所有者值须独立 |
| 管理窗口连接失败 | 核对管理器已运行、账户与普通权限一致、两处 manager-data-dir 相同，再刷新 |
| 项目地址返回不可用 | 在本机窗口核对是否已启动、暂停或退出；地址不会自动启动项目 |
| 请求认证失败 | 核对项目地址和对应 bearer；所有者凭据只用于审批入口 |
| 文件或命令被拒绝 | 查看 host_capabilities 和诊断，检查范围、只读、工具、身份、审批及后端是否可用 |
| 受限任务后端不可用 | 保留旁置 SandboxProbe，并检查声明的运行文件和启动验证结果 |
| 管理员不可用 | 检查项目许可、实际代理状态和 UAC 结果；重启不会自动恢复提权 |
| PDF 后端不可用或摘要不符 | 核对主程序旁的 pdfium.dll 与当前架构、固定版本一致；重新构建或从同一可信构建恢复依赖 |
| Office 格式或内容被拒绝 | 将旧格式、宏格式或加密文件转换成无宏、未加密的 DOCX / XLSX / PPTX；损坏或超过预算的包不能读取 |
| Office 预览与原文件页面不同 | 当前为结构化内容预览；原始排版渲染尚未实现 |
| 改配置后行为未变化 | 保存配置后停止管理器，切换配置并明确重启；窗口不在线重载 |
| 端口 8765 被占用 | 停止旧实例或在 gateway.port 中设置其他端口，同时更新客户端和已有代理转发 |

开发机中间编译文件、Python 字节码和 IDE 索引可重建；候选包、回退基线、验收报告、实际配置和 Data 应单独保留。测试源码及脚本已退出 Git 跟踪，本地副本保留；源码默认构建不依赖它们。
