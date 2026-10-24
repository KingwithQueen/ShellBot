# ShellBot Linux / Intel Mac 候选包

本包用于集中人工验收。版本为 Windows 1.0.3 对应的独立 POSIX 构建，当前构建号 17；不是 Windows 包，也没有改变 Windows 的安装和升级方式。`package-manifest.json` 的 channel 固定为 `acceptance-candidate`，人工验收状态固定为未完成。生成候选包不等于已发布、已公证或已通过真实客户端和系统授权验收。

## 校验与运行

先从可信交付记录核对 `.tar.gz.sha256`，再解压到当前用户控制的固定目录；不要用 sudo 解压，不要在共享可写目录运行。包内清单和 SHA-256 可以发现文件损坏，不能替代可信来源、数字签名或发布者提供的摘要。Python 3.9+ 仅用于下面的校验和菜单登记，原生程序运行不依赖 Python。

解压前也可以使用源码中 `PosixShellBot/distribution/verify-package.py --archive 包.tar.gz`，它只检查包结构和字节，不解压、不执行程序。解压后在候选包目录运行：

```sh
python3 verify-package.py
python3 verify-package.py --check-runtime
./bin/ShellBot --version
./bin/ShellBot --desktop
```

校验器拒绝符号链接、硬链接、特殊文件、共享可写文件、路径穿越、额外文件、内容或执行权限变化。运行检查另核对系统架构、共享库和版本，不会启动项目、访问凭据或安装服务。不要把 Data 或测试报告放进候选包；它们不属于只读程序负载。

## Linux x86_64

当前实机为 openSUSE Tumbleweed x86_64。包使用构建机器的动态运行库，不是跨所有发行版的静态通用包；`package-manifest.json` 的 requirements 记录实际链接的库和观察到的最低 GLIBC 符号版本。需要 GTK 4.10+、OpenSSL 3、Secret Service/libsecret、X11 输入相关库、GIO/GStreamer、Pango/Fontconfig、zlib 和 Expat；PDFium 随包提供。缺少库时先按当前发行版的包管理器安装对应运行依赖，再重做运行检查。不要盲目复制另一发行版的 libc，也不要为启动程序关闭系统安全机制。

普通命令的监督需要 Linux 5.3+、可用的 pidfd/subreaper 以及自身 `/proc` 信息；受限命令还依赖宿主允许的原生隔离，缺失时明确拒绝，不能回退为普通命令。实际桌面需要当前图形会话；Wayland 的共享授权、PipeWire 插件、桌面门户及输入能力由桌面环境决定，详见 `share/ShellBot/POSIX_REMOTE_DESKTOP.md`。

可选把本包登记到当前用户的应用菜单：

```sh
python3 install-desktop.py
python3 install-desktop.py --remove
```

这两个操作只创建或移除 `XDG_DATA_HOME/applications/org.shellbot.desktop.desktop`，默认位于 `~/.local/share/applications`。入口使用本包的绝对程序和图标路径，可处理空格及中文；移动目录前先移除旧入口，再在新位置登记。已有不同安装的入口、链接或不安全文件会保留并报错。此入口不是登录自启，操作不会复制程序、启动后台、安装/卸载服务或删除 Data。

`cmake --install` 中的通用 desktop 模板适用于把 `bin` 纳入 PATH 的标准前缀安装；解压包优先使用上述绝对路径菜单工具。CLI 用户也可以直接使用 `bin/ShellBot`，无需登记菜单。

## Intel Mac：macOS 13+

本轮仅支持 Intel x86_64，Apple Silicon 和旧系统尚未验收。可把整个 `ShellBot.app` 放到用户控制的固定应用目录，从 Finder 打开；或使用上面的 CLI 入口。CLI 和 App 各自携带匹配的辅助程序及 PDFium，不要拆分、替换其中组件。菜单栏、窗口和 Data 行为一致，CLI 与 App 的签名字节可以不同，必须以各自清单为准。

当前自动验收使用本地 ad-hoc 签名，它不提供 Developer ID 身份、公证或 Gatekeeper 发布信任。不要移除隔离属性或关闭 Gatekeeper 来把未验收包包装成正式发布。真实 Developer ID、时间戳、公证和隔离下载后的首次打开仍须由发布者使用自己的凭据完成，并对最终签名字节重新验收。

有证书时的显式构建选项是 `-DSHELLBOT_MAC_SIGN_IDENTITY="Developer ID Application: ..."`；本地无账号验证使用 `-DSHELLBOT_MAC_SIGN_IDENTITY=-`，省略选项则保持未签名开发构建。签名仅支持 Release、tests=OFF、sanitizers=OFF。真实身份模式启用 hardened runtime 和安全时间戳；ad-hoc 模式仅验证签名顺序，不冒充发布身份或 hardened runtime 验收。

构建先核对仓库原始 PDFium 摘要，签名构建副本，再把该副本的实际摘要编译进程序；先签名辅助程序，最后签名外层 App。不使用 `--deep` 递归重签库，不关闭 library validation，也不修改原始第三方文件。包生成后不得再独立重签 PDFium，否则摘要校验会拒绝加载。公证/staple 改变负载后须重新生成清单、包摘要并运行最终检查。

## 数据、升级和卸载

程序目录与应用/项目 Data 分开。已有配置、凭据、OAuth、审批和历史继续留在原 Data，不能通过覆盖程序包迁移或重置。先停止正在运行的项目、管理器及独立服务，保留旧程序包和 Data 恢复材料，再用新候选包打开同一显式 Data。切换程序位置前按 `POSIX_STARTUP.md` 和管理界面的服务页卸载旧登记，再为新程序重新预览、登记；只替换文件不会更新既有启动路径。

Linux 可先移除用户菜单入口，Mac 可移除该应用；程序目录删除前应停止进程并处理仍引用它的自启/服务。卸载程序不等于删除应用或项目 Data，不自动撤销已有凭据。Data 迁移必须使用管理界面的迁移入口，保留源数据，详见 `POSIX_PROJECTS.md`。不要手工移动、合并或复制加密 Data 来跨用户或跨系统升级。

原生凭据需要解锁的 Mac Keychain 或 Linux Secret Service；显式私有密钥模式不会自动替代失败的系统存储。更换签名身份后的 Keychain ACL、系统隐私授权和 root 安装所有权都须人工确认。

## 本机编辑、远端构建与候选包

Windows PowerShell 7.3+：

```powershell
.\scripts\build-posix.ps1 -Configuration Release -MacSignIdentity '-' -SshDirectory 'C:\Program Files\Git\usr\bin'
.\scripts\package-posix.ps1 -Run 构建收据中的run_id
```

前者选择显式源码输入、固定依赖与产品脚本，记录当前提交、未提交状态和逐文件 SHA-256；仅在指定 Mac/Linux 项目目录的隔离快照原生编译。后者拒绝源码变化，要求全新的生产 Release 构建、无诊断入口、正确版本/架构、完整许可和固定 PDFium；生成并验证 manifest、校验表和压缩包后取回本机。`-BuildVariant production` 可选择同快照中另行构建的生产目标；开启测试的 build 目标不能直接打包。

包生成程序也可在导出的原生源码快照运行：`python3 src/PosixShellBot/package.py --build-dir 绝对生产构建目录 --output-dir 新的外部输出目录 --source-revision 完整提交 --source-dirty true或false`。输出必须是新的目录，失败时保留准备材料，不自动覆盖旧包或清除数据。源码摘要绑定实际输入；基准提交加 working_tree_changes=true 不冒充干净提交构建。

所有候选包仍按 `share/ShellBot/POSIX_ACCEPTANCE.md` 集中人工验收。真实 root、Keychain/Secret Service 交互、外部 MCP/OAuth、Wayland/PipeWire、实体键鼠、多屏、登录/重启和长期运行没有被包校验替代。Mac 未观察孤儿、采样资源配额及 Wayland 已声明的协议限制继续有效。

签名流程依据 Apple 的 [Code Signing In Depth](https://developer.apple.com/library/archive/technotes/tn2206/_index.html)，菜单入口依据 [Desktop Entry 规范](https://specifications.freedesktop.org/desktop-entry-spec/latest/exec-variables.html)。
