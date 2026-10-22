# Linux / macOS 后台开发版：第一阶段

这是独立的 C17 POSIX 后台入口，不是 Windows 正式包或完整桌面版。正式 Windows 1.0.3 的 MSBuild 入口和 Win32 后端继续使用原有构建方式。当前已使用 Linux x86_64 和 Intel macOS 实机通过 SSH 编译运行；不能据此宣称所有发行版、旧系统或 Apple Silicon 已验收。

## 本阶段功能

- stdio 和仅绑定 `127.0.0.1` 的 HTTP；复用已有 MCP 协议调度。
- 一个进程固定绑定注册表中的一个项目，项目策略、只读、工具开关、审批、审计与历史状态继续复用共享核心。
- 文件列举、UTF-8/UTF-16 读取与分页、名称/内容搜索、创建文本、按摘要覆盖与替换。
- `fs_stat`、创建目录、文件复制、同文件系统移动、文件删除及空目录删除；拒绝覆盖目标、目录树复制和递归删除。
- 本地 Unix socket 审批通道，由 Linux `SO_PEERCRED` 或 macOS `getpeereid` 核对同一用户。支持 inspect/approve/deny/revoke、审批列表和停止 HTTP 后台。
- 审批与状态使用原有持久化模型，重启保留历史，未完成操作记录中断，不自动重放或恢复旧许可。

命令执行、受限/管理员进程、OAuth 与凭据存储、产物传输、图像/PDF/Office、桌面控制、管理界面、托盘、自启和服务安装尚未实现。工具发现隐藏这些能力，直接调用也会拒绝；注册表不能开启执行、管理员、桌面或账户全盘范围。每个后台进程只有一个固定项目，项目管理器和自动启停留到下一阶段。

## 原生构建

依赖：C17 编译器、CMake 3.21 或更新版本、线程库，推荐 Ninja。yyjson 和现有带本地补丁的 CivetWeb 直接从仓库编译，不下载运行依赖，也不使用 Windows PDFium DLL。

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

示例默认只读且每次审批。需要写入时，由所有者明确将相应工作目录的 `read_only` 改为 `false`。`approval_mode:workspace_auto` 仅自动批准已授权工作目录内的文件操作；`always` 保留逐次本地审批。配置由进程启动时固定加载，修改后停止对应进程再重启，不宣称已有管理器热更新或 OAuth 会话撤销机制。

HTTP 使用独立进程环境中的令牌，访问 `/mcp` 或 `/projects/example/mcp`；`/healthz` 只报告存活。令牌要求 32～128 个十六进制字符，可使用内置生成器：

```sh
export SHELLBOT_EXAMPLE_TOKEN="$(build/posix-native/ShellBot --generate-token)"
build/posix-native/ShellBot --http --projects /absolute/private/projects.json --project example
```

`Authorization: Bearer ...` 在请求体和工具调用前校验；请求体最多 1 MiB，必须具有明确 Content-Length，JSON 响应上限继续使用共享核心限制。拒绝浏览器 Origin、未知 Host、重复认证头和查询参数。需要公网地址时沿用用户自己的隧道，ShellBot 不修改代理或域名。本阶段 HTTP bearer 不是 OAuth 长期授权实现。

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

现有约定将测试源保留在本机而不纳入 Git。恢复本地 `tests/posix_integration.py` 后可增加 `-WithTests`；测试仅在各远端项目下独立 `.qa-*` 夹具中读写，保留报告和备份，退出自己启动的进程。报告摘要须与取回的程序一致才视为通过。`-Sanitizers` 可对后台与共享核心启用地址/未定义行为检查；不声称第三方静态库也已经插桩。

本阶段的 SSH、原生后台、隔离文件与 HTTP 测试不等于真实 MCP 客户端、OAuth、GUI、物理桌面或跨发行版发布验收。
