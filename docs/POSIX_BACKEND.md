# Linux / macOS 后台开发版：第四阶段

这是独立的 C17 POSIX 后台入口，不是 Windows 正式包或完整桌面版。正式 Windows 1.0.3 的 MSBuild 入口和 Win32 后端继续使用原有构建方式。当前已使用 Linux x86_64 和 Intel macOS 实机通过 SSH 编译运行；不能据此宣称所有发行版、旧系统或 Apple Silicon 已验收。

## 本阶段功能

- stdio 和仅绑定 `127.0.0.1` 的 HTTP；复用已有 MCP 协议调度。
- 一个进程固定绑定注册表中的一个项目，项目策略、只读、工具开关、审批、审计与历史状态继续复用共享核心。
- 文件列举、UTF-8/UTF-16 读取与分页、名称/内容搜索、创建文本、按摘要覆盖与替换。
- `fs_stat`、创建目录、文件复制、同文件系统移动、文件删除及空目录删除；拒绝覆盖目标、目录树复制和递归删除。
- 本地 Unix socket 审批通道，由 Linux `SO_PEERCRED` 或 macOS `getpeereid` 核对同一用户。支持 inspect/approve/deny/revoke、审批列表和停止 HTTP 后台。
- 审批与状态使用原有持久化模型，重启保留历史，未完成操作记录中断，不自动重放或恢复旧许可。
- 本地项目列表、新增/配置/删除导出、启停、暂停/恢复与状态；管理器为每个启动的项目创建独立工作进程，统一回环 HTTP 按项目路由、分别认证。
- 普通当前账户命令：启动、状态、二进制输出分页、标准输入及 EOF、取消、超时、输出限制、任务历史和输入回执。

受限/管理员进程、产物传输、图像/PDF/Office、桌面控制、自启和服务安装尚未实现。媒体工具继续隐藏；受限执行、管理员和桌面控制配置在启动时拒绝。普通命令须由项目显式开启；注册项目不会自动启动，异常退出的工作进程也不会自动重启。

第三阶段加入 Keychain / Secret Service 加密凭据、显式私有密钥文件模式和管理器 OAuth；参见 [凭据存储与 OAuth](POSIX_OAUTH.md)。

第四阶段加入 Mac Cocoa / Linux GTK 4 原生项目窗口、审批与 OAuth 连接管理，以及菜单栏/StatusNotifier 托盘；参见 [管理界面与托盘](POSIX_DESKTOP.md)。界面默认构建；无桌面环境可明确设置 `-DSHELLBOT_DESKTOP=OFF`。

## 原生构建

依赖：C17 编译器、CMake 3.21 或更新版本、线程库，推荐 Ninja。Linux 另需 OpenSSL 3 和 libsecret 的开发包与运行库，界面需 GTK 4.10+；Mac 使用系统 Security/CoreFoundation/CommonCrypto，界面另用 Cocoa 和 Objective-C 编译器。yyjson 和现有带本地补丁的 CivetWeb 直接从仓库编译，不下载运行依赖，也不使用 Windows PDFium DLL。

```sh
cmake -S . -B build/posix-native -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/posix-native --parallel 4
build/posix-native/ShellBot --version
```

在 Mac 上使用已安装的 Apple 开发工具链。构建默认针对当前机器架构；跨架构编译不能代替相应硬件运行验收。Linux 二进制依赖构建机器的 libc，不是面向旧发行版的通用包。

可选 `cmake --install build/posix-native --prefix /absolute/private/install-dir` 会安装命令行程序和 ShellBot、yyjson、CivetWeb 许可，不注册系统服务。

## 配置与启动

复制 `config/posix-projects.example.json` 到本机私有配置文件，替换两个绝对目录。Mac 使用 `/Users/...`，Linux 使用 `/home/...` 等目标机器实际目录。工作目录必须已经存在；私有数据目录的父目录也必须存在。数据目录须属于当前用户、权限为 `0700`，且不能与任何暴露的工作目录重叠；新建的数据目录会自动使用 `0700`。

```sh
build/posix-native/ShellBot --validate-projects /absolute/private/projects.json
build/posix-native/ShellBot --stdio --projects /absolute/private/projects.json --project example
```

文件示例默认只读且每次审批。需要写入时，由所有者明确将相应工作目录的 `read_only` 改为 `false`。`approval_mode:workspace_auto` 仅自动批准已授权工作目录内的文件操作；`always` 保留逐次本地审批。配置由进程启动时固定加载；管理器内重启项目仍使用本次管理器的私有注册表快照。修改注册表后应重启管理器，不支持热更新。

HTTP 使用独立进程环境中的令牌，访问 `/mcp` 或 `/projects/example/mcp`；`/healthz` 只报告存活。令牌要求 32～128 个十六进制字符，可使用内置生成器：

```sh
export SHELLBOT_EXAMPLE_TOKEN="$(build/posix-native/ShellBot --generate-token)"
build/posix-native/ShellBot --http --projects /absolute/private/projects.json --project example
```

`Authorization: Bearer ...` 在请求体和工具调用前校验；请求体最多 1 MiB，必须具有明确 Content-Length，JSON 响应上限继续使用共享核心限制。拒绝浏览器 Origin、未知 Host、重复认证头和查询参数。需要公网地址时沿用用户自己的隧道，ShellBot 不修改代理或域名。管理器可显式开启 OAuth，配置与授权流程见 [凭据与 OAuth](POSIX_OAUTH.md)。

端口仍接受 1～65535。低端口由目标操作系统决定是否允许普通用户监听，程序保留系统拒绝，不添加 1024 下限，也不自动提权。

## 本地所有者审批

工具返回 pending 后，所有者在目标机器另一个终端操作；远端调用方不能通过文件工具批准自身请求。

```sh
build/posix-native/ShellBot --list-approvals --data-dir /absolute/private/ShellBotData
build/posix-native/ShellBot --inspect APPROVAL_ID --data-dir /absolute/private/ShellBotData
build/posix-native/ShellBot --approve APPROVAL_ID --data-dir /absolute/private/ShellBotData
```

检查后重试完全相同的工具参数并附上 `approval_id`。需要将决定绑定已检查的请求摘要、实例和策略版本时，使用 `--owner-request FILE --data-dir ...` 发送现有 owner JSON 格式；允许的 expected 字段与 Windows 共享核心相同。

`--stop --data-dir ...` 停止 HTTP 后台；stdio 由 EOF、SIGINT 或 SIGTERM 退出。关闭审批 socket 不代表撤销文件效果。Unix socket 有系统路径长度限制；过长的数据目录会在启动时明确报错，应选择较短的私有路径。

## 项目管理与独立工作进程

管理器和各项目分别使用私有 `0700` 数据目录，不能互相重叠，也不能与任何配置的文件根目录重叠。用目标机器的短绝对路径替换以下示例。HTTP 模式下，所有启用的项目须配置互不相同的 bearer；管理器只监听一个端口，工作进程不另开公开监听器。

```sh
ShellBot --project-list /private/projects.json
ShellBot --manager --http --projects /private/projects.json --manager-data-dir /private/ManagerData --start-project example
ShellBot --management-status --manager-data-dir /private/ManagerData
ShellBot --project-start another --manager-data-dir /private/ManagerData
ShellBot --project-pause example --manager-data-dir /private/ManagerData
ShellBot --project-resume example --manager-data-dir /private/ManagerData
ShellBot --project-status example --manager-data-dir /private/ManagerData
ShellBot --project-stop example --manager-data-dir /private/ManagerData
ShellBot --manager-stop --manager-data-dir /private/ManagerData
```

省略 `--http` 可仅运行本地管理后台。只有 `--start-project ID` 或本地启动命令会启动项目。访问 `/projects/ID/mcp` 使用该项目凭据；管理器的 `/mcp` 只有配置了 `gateway.default_project_id` 时才有路由。每个工作进程固定项目、策略版本、Data 和实例编号，工具参数不能切换其身份。暂停先阻止路由并取消已有命令，恢复沿用当前实例；停止后显式启动产生新实例。另一个项目保持独立运行。

同一用户的本地管理通道可以转发项目审批或历史请求：

```sh
ShellBot --project-owner example --request /private/owner-request.json --manager-data-dir /private/ManagerData
```

`owner-request.json` 使用现有 owner 格式，例如 `{"command":"list"}` 或 `{"command":"history","id":"任务编号"}`。项目自己的 `--approve ID --data-dir PROJECT_DATA` 也继续可用。

新增与配置文件为单个项目 JSON 对象。导出始终创建新注册表并保存原始字节备份；不覆盖旧文件，不在线改动运行中策略：

```sh
ShellBot --project-create /private/new-project.json --projects /private/projects.json --output /private/projects-next.json --backup /private/projects-original.json
ShellBot --project-configure example --project-definition /private/changed-project.json --projects /private/projects.json --output /private/projects-next.json --backup /private/projects-original.json
ShellBot --project-remove example --projects /private/projects.json --output /private/projects-next.json --backup /private/projects-original.json
```

每次使用新的输出和备份路径。备份成功而输出失败时保留备份供检查。配置命令沿用共享核心的策略版本递增和注册表验证规则；下一次管理器启动时加载新的注册表。

## 普通命令与任务边界

`config/posix-commands.example.json` 是独立的命令示例，默认逐次审批。它显式授予 `execution.enabled:true`、`default_identity:current_user`、`files.scope:current_user` 和 `current_user_read_only:false`；任何配置目录为只读时，普通命令仍按原策略拒绝。配置根目录范围的项目不能借普通命令绕过目录边界。不会自动扩大现有项目许可，也不会调用 sudo 或请求管理员授权。

先调用 `host_capabilities` 取得 `execution_status.host_instance_id`。所有 `task_*` 请求携带该编号，`task_start` 另带工作目录 ID、程序绝对真实路径、`argv` 数组、`request_key` 和可选超时。程序须为原生 ELF/Mach-O，拒绝路径中的符号链接、多硬链接、set-id 文件及直接脚本；脚本应通过明确的解释器及 argv 执行。不会搜索 PATH 或自动拼接 shell。需要 shell 时由调用方明确选择原生 shell 程序并承担其当前账户权限。

```json
{"workspace_id":"workspace","path":".","executable":"/usr/bin/uname","argv":["-a"],"request_key":"inspect-system-1","timeout_ms":10000,"host_instance_id":"能力查询返回的32位编号"}
```

任务启动前保存意图及审计，再交由任务监督进程执行；保存失败时阻止新命令和 stdin。相同请求键返回原任务，内容变化拒绝；跨实例保留的键阻止自动重放。审批固定程序摘要和目录/程序版本；Linux 使用已打开文件执行，macOS 在校验后按原生路径执行，不宣称抵御恶意同 UID 修改程序的所有竞争。解释器读取的脚本、依赖和网络资源没有被自动固定或隔离。

`task_output` 返回 `data_base64`、字节偏移及 `next_offset`；有效 UTF-8 页另带 `text_utf8`。stdout 和 stderr 合计保留不超过项目 `max_output_bytes`，超限停止任务。默认最多 8 个活动任务、32 条实例内记录、256 KiB 输出；记录不自动淘汰，避免旧请求键被再次执行。`task_input` 每次最多 4096 字节 UTF-8，携带 `input_key` 和可选 EOF；每任务最多 16 份回执，同键不同内容拒绝。状态区分排队、已投递和失败；部分写入字节数保留供核对。

任务只继承系统生成的 HOME/USER/LOGNAME、固定 PATH、LANG 和 TMPDIR，不继承宿主 bearer 或自定义环境变量。当前实例的输出留在受限内存缓冲区；历史保留每流最多 8192 字节前缀与截断标记，以及命令元数据和 stdin 投递回执。任务与审计 Data 沿用私有权限保护；项目凭据及 OAuth 状态可使用第三阶段独立的加密存储。

取消、超时、暂停和正常退出会停止任务进程组中的普通后代。工作进程崩溃时独立监督进程检测通道关闭并清理进程组；管理器异常退出后，工作进程因私有通道关闭而退出并停止任务。没有按进程名批量终止其他程序，也不自动恢复任务或 stdin。主动脱离进程组的守护进程、恶意同 UID 程序、监督进程本身被强杀均不属于已验证的完整任务树隔离。

`worker_memory_mb` 在 Linux 当前实现中用作单条命令的地址空间限制，排除监督进程和工作进程；macOS 暂不执行该内存限制，能力响应会明确标注。两平台都有任务数、输出、输入、超时及关闭 core dump 的限制；不能把它们当作账户权限或整个进程树资源沙箱。

## 文件边界与限制

根目录及相对路径逐段通过目录描述符和 `openat` 定位，拒绝符号链接、`..`、空路径分量、特殊文件和多硬链接普通文件。根目录还按物理身份检查重叠，不能依靠大小写差异绕过同一卷的别名检查。根路径自身也不能经过符号链接，因此 macOS 的 `/tmp`、`/var` 等别名路径应使用真实路径或用户目录。目录中不能解码为受支持 Unicode 文本的名字会使列举报错。

操作复查设备号、inode、链接数、大小及纳秒时间戳；创建和复制通过排他创建避免覆盖，移动使用 Linux no-replace rename 或 macOS `RENAME_EXCL`。修改按原契约先备份，再持锁原地写入，不声称原子替换；文件删除先保留私有原始字节。文本编辑上限 64 KiB，文件复制/删除上限 64 MiB，每个实例最多创建 64 份备份。

POSIX `flock` 是协作式锁。目录定位与版本复核是文件工具的访问边界，不是阻止同账户其他程序写入、移动目录或执行命令的系统沙箱；无法消除恶意同 UID 进程在路径变更或原地编辑中的所有竞争。未完成跨 UID 对抗、突然断电、全盘故障、所有挂载类型和长期运行验收。读写失败、备份和审计收据应保留用于恢复；不要自动清理私有 Data。

## 本机编辑、SSH 构建

在 Windows PowerShell 7.3 或更新版本运行 `scripts/build-posix.ps1`。将 `config/remote-build.example.json` 复制到被忽略的私有配置位置，填写已验证的 SSH 主机、项目目录、专用密钥和固定 known_hosts；不把私钥或密码写入仓库。

```powershell
.\scripts\build-posix.ps1 -ConnectionFile .\config\remote-build.local.json -Configuration Release
```

脚本仅选择 CMake、POSIX 后端、共享 C 源码及两个源码依赖，包含本机未提交的这些源码改动；不复制 `.git`、Windows 产物、Data 或 SSH 密钥。每次在远端指定根目录的 `.native-build/时间-随机值` 下新建源码和构建快照，核对逐文件 SHA-256 后编译，不覆盖现有文件，不运行安装操作。结果和取回的二进制保存在本机 `build/posix`，历史失败保留。

现有约定将测试源保留在本机而不纳入 Git。恢复本地 `tests/posix_integration.py`、`posix_phase2.py`、`posix_oauth.py`、`posix_clock.c`、`posix_desktop.py` 和 `posix_tray.py` 后可增加 `-WithTests`。后台与界面控制器测试仅在远端项目下独立夹具中读写，保留报告和备份，退出自己启动的进程；托盘模拟和原生窗口检查单独运行。两份功能报告摘要须与取回的程序一致才视为通过。`-Sanitizers` 对后台、控制器与共享核心启用地址/未定义行为检查；不声称系统界面库及第三方静态库已插桩。

本阶段的 SSH、原生后台、隔离文件与 HTTP 测试不等于真实 MCP 客户端、OAuth、GUI、物理桌面或跨发行版发布验收。
