# Linux / macOS 登录自启与后台服务

第七阶段提供原生安装、状态、启停和卸载入口。只修改 Linux/macOS 入口，Windows 继续用原 MSBuild 与服务实现。

|方式|Linux|macOS|启动时机|
|---|---|---|---|
|desktop|XDG autostart .desktop|用户 LaunchAgent|图形登录后打开管理界面，不自动启动后台/项目|
|user-service|systemd --user|用户 LaunchAgent|用户登录后的独立后台；不依赖管理窗口存活|
|system-service|系统 systemd，User=指定普通账户|系统 LaunchDaemon，UserName=指定普通账户|系统启动，需管理员安装；不会以 root 运行后台|

管理界面的“连接设置”中有启用/取消登录自启和当前状态。取消只移除下次登录登记，当前窗口与数据保留。第十三阶段加入独立的“服务”页，可配置、预览、安装、启停和卸载用户/系统后台服务；下面的命令行入口仍可使用。退出连接到已有服务的管理窗口会保留服务，手动停止后台会正常退出，不立即重新启动。

## 原生界面管理后台服务

后台服务与打开窗口的“登录自启”分别登记。用户后台在登录后独立运行；系统后台在开机时以当前普通账户运行。填写服务名称、独立的安装状态目录、密钥文件、是否启用 OAuth 和明确的项目启动列表，先保存并生成预览，核对原生 unit/plist 的位置、完整参数、运行用户和启动列表，再明确安装。预览不会创建登记或启动后台；安装只登记，启动另行操作。后台的注册表和普通 Data 使用当前窗口的活动位置。服务所需的现有加密凭据应提前准备；界面不会把 bearer 或密码放进配置中。

安装必须对应本次已查看的完整预览。更改表单、注册表、项目启动列表或程序参数后，旧预览立即失效。启停、卸载绑定实际登记的 unit 标识和内容摘要；外部替换、修改、链接、同名他人登记和不一致记录均拒绝控制，不按名称猜测归属。已经登记的服务不能直接改为另一个目录或另一组参数；先明确卸载自己的旧登记，再保存新设置。停止和卸载会结束该服务的项目及任务，日志、收据、配置、凭据、项目文件和 Data 保留。

用户后台使用当前账户的原生系统工具。系统后台的每次安装、启停、卸载和状态刷新分别请求系统管理员授权：Linux 使用 polkit 的 `pkexec`，Intel Mac 使用系统管理员对话框。程序不接收或保存密码；拒绝、取消或超时后不自动再次弹窗。系统状态是上次明确刷新的结果，界面显示检查时间和错误；普通窗口刷新不会静默请求管理员权限。系统后台仍以当前普通账户运行，安装授权不授予项目管理员文件或命令能力，也不接管“管理员”页的授权会话。

系统服务卸载后仍保留 root 私有的安装历史目录，普通窗口可能无法直接确认其中是否还有登记。明确检查得到的“未登记”结果仅在当前窗口中保留 30 秒的编辑许可；过期、重开窗口或检查失败后，如不能直接确认登记不存在，修改设置或迁移前需要再次点击“检查状态”并完成该次系统授权。定期刷新不会代替这一步或自动弹出授权。

系统服务要求受保护的完整安装和 root 拥有且不可被普通账户改写的安装状态祖先目录；普通账户可写的开发程序会在系统授权前拒绝。首次准备这些目录仍由所有者完成，不能把现有普通 Data 改为 root 所有。启用系统服务需明确提供当前普通账户拥有的私有 key-file；实际用户目录、凭据、注册表和项目路径须在开机时可用。

服务运行后重新打开管理窗口会连接已有后台，窗口不会取得该进程的所有权；关闭或退出窗口保留服务。界面的普通“启动后台”不会接管或覆盖已登记的服务，服务启动也不会把已存在的独立后台悄悄改为服务管理。需使用对应的明确服务操作。服务仍登记或运行时，配置编辑及项目/应用 Data 迁移均被阻止，以免留下指向旧位置的启动项；先停止并卸载登记，再迁移和重新预览、安装。服务启动列表独立于普通窗口的项目“随后台启动”选项；两者都不恢复旧任务、输入或审批。

## 用户级后台：SSH 可操作

先将完整开发包放到稳定位置；不要从即将删除的编译目录安装。下面 `ShellBot` 代表该位置下的实际可执行文件（Mac 也可用 app 内 Contents/MacOS/ShellBot）。所有路径必须是规范绝对路径，不接受符号链接、点或父目录分量。先准备自己的项目注册表和私有数据、凭据目录。

```sh
ShellBot --startup-plan --kind user-service --name my-backend \
  --startup-dir /absolute/private/startup \
  --projects /absolute/projects.json --manager-data-dir /absolute/private/backend \
  --credential-key-file /absolute/private-keys/master.key --start-project example
```

`--startup-plan` 只输出安装位置、完整参数和原生描述，不写入、不启动。核对后把它换成 `--startup-install`；安装登记未来启动，但不立即启动。然后：

```sh
ShellBot --startup-start --startup-dir /absolute/private/startup
ShellBot --startup-status --startup-dir /absolute/private/startup
ShellBot --startup-stop --startup-dir /absolute/private/startup
ShellBot --startup-uninstall --startup-dir /absolute/private/startup
```

命令行还可添加 `--expected-runtime-uid`、`--expected-kind`、`--expected-unit-id` 和 `--expected-content-sha256`，把操作绑定到已核对的运行账户、类型、unit 和描述摘要；存在登记时，在同一次归属锁下匹配后才执行。plan/install 另外支持 `--expected-registry-sha256`，要求项目配置的原始内容摘要仍与预览一致。原生界面使用这些保护；直接调用命令行时可按既有用法省略，但它们不能替代系统授权或文件归属校验。

只有明确的 `--start-project ID` 项目会随服务启动，可重复给出不同项目；省略时仅启动管理后台。启动/恢复仍按项目当前配置校验，注册表不是历史运行列表。服务不会重放旧命令、任务输入或已中断的审批。`--oauth` 要求注册表已有 HTTPS public_url；隧道、TLS 和域名继续由所有者维护。

服务使用既有加密凭据存储，不把 bearer 放进原生配置、命令行或环境文件。可先用 `--credential-key-create` 和 `--credential-set/--credential-generate` 准备隔离的私有密钥及项目凭据，详见 POSIX_OAUTH.md；不要在命令行写令牌。用户服务可省略 key-file，使用已解锁的 Keychain/Secret Service；服务环境未解锁或缺少用户总线时会明确失败，不弹窗索取密码或降级明文。SSH 和系统开机服务建议显式私有 key-file，64 字节文件和父目录必须分别为该账户的 0600/0700。密钥目录不能向项目暴露。

Linux 的用户 systemd 需要可用的用户管理器，安装会执行 daemon-reload 和 enable，启动单独执行 start。当前 SSH 会话通常有 /run/user/UID 用户总线。用户服务不等于无人登录时开机运行；若所有者选择 linger，须自行以管理员权限配置和验收，本程序不自动修改 linger。真正的系统开机后台使用 system-service。

Mac 用户服务的操作目标为 gui/UID，SSH 可在该账户已有图形登录会话时管理。只有 SSH 登录而没有图形域时，可以生成和安装 LaunchAgent，start 会明确返回域不可用；不伪造图形会话。需要无人登录时运行的后台使用 system-service。

## 系统级开机后台

把上面的 kind 换成 system-service，并添加 `--service-user owl`。plan 可由普通用户生成；install/start/stop/uninstall 必须由管理员通过 sudo 执行。指定的运行用户必须存在且不是 root；必须明确提供该用户拥有的私有 credential-key-file。安装状态目录及其所有祖先必须由 root 拥有且不可被组或其他账户写入，不能放在普通用户的 home 或 /tmp 下；这也保护 launchd 降权前打开的日志路径。安装状态独立于后台普通用户的数据目录；Mac 可选 /Library/Application Support/ShellBotStartup/name，Linux 可选 /var/lib/ShellBotStartup/name（父目录先由管理员准备）。不要修改既有 Data 的所有者来迁就安装。

系统 unit/plist 只赋予普通账户启动权限。受限命令仍须通过实际隔离探针；管理员命令另需本地所有者显式启动匹配项目的提权代理，服务安装不授予管理员命令能力，见 POSIX_COMMANDS.md。没有图形会话时，屏幕与键鼠、系统密钥串和用户总线等仍受系统限制。系统启动前必须已挂载运行程序、注册表、密钥和项目路径；加密用户目录、FileVault、网络挂载的实际时序需人工验收。系统服务不能绕过这些边界。

## 登记归属和恢复

安装状态目录必须私有且由安装账户拥有。先保存 startup.json 归属记录，再创建原生文件；崩溃或系统工具失败时保留记录，可按同样参数重试或卸载。若 Linux 描述丢失但服务或启用链接尚存，卸载会先独占恢复归属记录中的原文，再停止并取消登记。相同参数安装幂等；不同参数、同名外部文件、被改动或链接的文件会拒绝覆盖或删除。升级时保持程序的稳定完整位置；需要变更参数时先卸载登记，再用新参数安装。卸载先停止自己的后台（会结束项目及运行中的任务），再移除自己的登记，保留归属历史、日志、项目文件、配置、加密凭据与运行记录。不递归清除任何数据目录。

Linux unit 使用 Restart=on-failure、15 秒延迟和启动速率限制，正常退出不重启。Mac 使用 SuccessfulExit=false 的 KeepAlive 和 15 秒节流。崩溃后只按明确的启动列表重新启动；不会自动重启异常退出的单个项目工作进程。Mac 后台 stdout/stderr 写入安装状态目录，系统级日志由管理员私有持有；Linux 由 journal 保存。原生状态不等于 HTTP 已就绪，凭据/路径/端口错误可通过原生日志和 --management-status 查明。

自动测试使用唯一名称的临时用户服务和独立加密测试凭据，结束后卸载。系统级描述经过原生语法验证和非管理员拒绝测试；真实系统级安装、重启/登录时序、实际桌面和外部客户端仍待人工验收。没有执行重启、注销、linger 变更或安装真实项目。

原生机制依据：[systemd.service](https://www.freedesktop.org/software/systemd/man/latest/systemd.service.html)、[桌面自启规范](https://specifications.freedesktop.org/autostart/latest/)、[Apple launchd](https://developer.apple.com/library/archive/documentation/MacOSX/Conceptual/BPSystemStartup/Chapters/CreatingLaunchdJobs.html)。
