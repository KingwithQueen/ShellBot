# Linux / macOS 原生管理界面

原生界面于第四阶段接入，第九阶段补齐完整项目表单、独立远程所有者审批与数据迁移（见 POSIX_PROJECTS.md）。在 Windows 编辑同一份源代码，通过 SSH 在 Linux 和 macOS 原生编译、测试。Mac 使用 Cocoa 窗口和菜单栏状态图标；Linux 使用 GTK 4 窗口和 StatusNotifier 托盘。原有 Windows MSBuild、Win32 界面和后台继续独立构建。

## 打开与首次配置

Mac 打开生成的 `ShellBot.app`。Linux 运行 `bin/ShellBot --desktop`，安装后也可使用应用菜单中的 ShellBot。无参数启动有界面构建的程序也会打开窗口；现有 `--stdio`、`--manager` 等后台命令保持原有行为。

默认使用以下私有目录保存界面配置、备份和后台记录：

- Mac：`~/Library/Application Support/ShellBot`。
- Linux：`$XDG_DATA_HOME/ShellBot`，默认 `~/.local/share/ShellBot`。

可用 `--desktop --data-dir /absolute/private/directory` 指定独立目录；末级目录不存在时创建为 `0700`，父目录需存在。已有目录必须属于当前账户且禁止其他用户访问。Unix socket 有平台路径长度限制，过长时请明确选择较短的私有目录。程序不会读取或迁移 Windows Data。

1. 点击“添加”，填写项目标识并选择工作目录。新项目默认只读。
2. 需要写文件时选择“可修改所选目录”；需要普通命令时明确选择“当前账户文件与普通命令”。三种表单方案都保留每次操作审批。命令以当前账户运行，工作目录不是命令沙箱。
3. 在“连接设置”设置本机端口。OAuth 另需明确填写 HTTPS 公网地址并勾选启用；实际隧道由用户配置，后台仍只监听回环地址。
4. 点击“启动后台”，再选择项目并“启动项目”。不自动启动项目，不重放旧任务。暂停项目只阻止新请求，不暂停已开始的命令；停止项目或后台会结束其命令。

HTTP 所需项目凭据会在后台停止时自动生成并加密保存，已有凭据不会轮换。默认主密钥由 Mac Keychain / Linux Secret Service 保护，界面不会显示 token 或请求解锁密码；系统钥匙串不可用时会明确报错。先通过系统工具解锁，或为独立开发环境显式指定 `--credential-key-file`；不会自动降级为明文或生成替代密钥。详情见 [凭据与 OAuth](POSIX_OAUTH.md)。

“导入”先验证再保存注册表副本，原配置和原 Data 保留。全部目录、策略、运行资源、工具、配额和自动启动选项可以在六页原生项目表单编辑。已有地址标识与凭据引用保持固定；变更项目 Data 或应用 Data 使用专用迁移入口，参见 [完整配置与数据迁移](POSIX_PROJECTS.md)。

保存项目、导入、移除和修改连接设置均须先停止后台。每次保存产生独立备份；后台仍使用启动时的注册表快照，修改后重新启动生效。各项目明确选择的“随后台启动”选项只影响本机管理窗口启动后台的行为，不会重放旧任务。移除项目不删除工作文件、Data 或旧凭据。

## 审批、连接和退出

“审批与连接”显示当前所选项目的操作审批，以及管理器的 OAuth 请求、连接和客户端。先“查看完整操作”核对目标和内容，再允许或拒绝；决定绑定已查看的摘要、运行实例和策略版本，不代替客户端重试，也不会自动执行操作。审批列表刷新不会自动批准。

OAuth 申请展示客户端名称、回调地址、项目和当前权限。持续授权默认不勾选；明确勾选后，授权持续到手动撤销，并包括项目今后允许的操作。连接授权与每次操作审批、文件范围、命令权限分别生效。可撤销连接或删除客户端，撤销不回滚已完成的操作。

关闭窗口保留后台，可从 Mac 菜单栏图标或 Linux 托盘重新打开。“退出 ShellBot”会停止本次由界面启动的后台、工作进程和命令。重新打开界面连接到既有后台时，退出只关闭界面；明确“停止后台”仍会停止该后台。异常退出后不会自动重放任务。每个界面私有目录只允许一个实例，第二次启动请求已有窗口恢复。

Linux 支持 KDE 等提供 `org.kde.StatusNotifierWatcher` 的桌面。托盘服务更换后重新注册；托盘宿主消失时恢复窗口。GNOME 未安装相应扩展等无托盘环境中，关闭按钮保留窗口并提示最小化或明确退出，防止后台运行而无法找回。未实现旧 XEmbed 托盘。窗口、项目与审批操作由后台队列执行，系统界面线程不等待本机 IPC。

## 构建和验证

界面默认启用。Linux 另需 GTK 4.10+（开发包和运行库）及桌面会话 D-Bus；Mac 需 Apple 开发工具和 Cocoa，最低目标为 macOS 13。沿用原有图标，构建时用系统工具生成 Mac `.icns`。仅后台环境可设置 `-DSHELLBOT_DESKTOP=OFF`，不构建 GTK 窗口，Mac 远程桌面仍使用 Cocoa 系统 API。

```sh
cmake -S . -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/native --parallel 4
cmake --install build/native --prefix /absolute/staging-directory
```

安装到指定目录只生成文件，不注册服务、自启、钥匙串解锁或系统应用关联。Linux `.desktop` 需要 `ShellBot` 在应用启动环境的 PATH 中；未安装时直接打开暂存目录的程序。Mac 开发包未签名、公证或制作通用架构包；Linux 二进制依赖构建机的 libc、GTK、OpenSSL、libsecret 等系统库。

`SHELLBOT_TESTS=ON` 额外启用隔离控制器驱动和短时界面检查入口。它们使用生成的项目和凭据，不连接真实 MCP 账户；正式打包可关闭此选项。后端回归、界面自动化、托盘协议模拟、实际桌面会话和人工操作验收须分别记录。第五阶段加入独立的“远程桌面”页和托盘暂停/恢复入口，参见 [远程桌面](POSIX_REMOTE_DESKTOP.md)。后续阶段的媒体工具、自启服务和命令身份分别见 POSIX_MEDIA.md、POSIX_STARTUP.md、POSIX_COMMANDS.md；真实外部客户端与实际桌面仍需人工验收。
