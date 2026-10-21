# 常驻与自动启动

版本：`0.1.0-dev.19`，M12。更新：2026 年 10 月 17 日。同时提供服务和托盘两种模式。两者使用同一项目管理、权限、审批及历史后端，各自保存明确的启动配置，不自动恢复旧批准、旧任务或文件传输。

按用户最新确认，自启验收以 Windows 实际登记和回读的启动项、计划任务或服务设置为准，不要求重启或注销验证。Windows 负责触发和已配置的重启策略；程序路径、配置加载、权限或依赖错误仍由 ShellBot 提供诊断，不能一概归为 Windows 故障。

## 选择模式

| 模式 | 运行时机 | 身份与适用范围 |
| --- | --- | --- |
| 托盘 | 当前用户登录后；也可手动启动 | 普通启动使用普通管理器；以管理员身份启动时复用已有权限创建共享代理，普通工作进程仍用普通令牌。关闭窗口隐藏到托盘；退出停止本实例及其活动任务 |
| 管理员托盘登录任务 | 当前账户已登录，Windows 计划任务触发 | 同账户、交互登录、最高权限；首次登记须已提升的所有者。之后由 Windows 启动，无需再通过 ShellBot 请求 UAC；不存储 Windows 密码 |
| Windows 服务 | 开机自动启动，默认延迟启动；无需用户登录 | 独立虚拟账户 `NT SERVICE\ShellBotService`，在 Session 0 提供授权文件工具和通过后端核验的受限任务；不提供桌面、剪贴板或自动管理员授权 |

Windows 服务位于非交互会话，不能当作登录桌面的控制器，见 [Microsoft 交互服务说明](https://learn.microsoft.com/en-us/windows/win32/services/interactive-services)。服务账户不等于配置所有者的普通 Windows 账户；目录和程序必须允许该服务账户访问。服务配置限定本地 configured 文件范围，拒绝 legacy_config、current_user 文件范围、桌面、管理员许可及账户任意命令。服务要求独立 owner_token_env，以便通过既有所有者页面处理审批。

同一台电脑可分别配置两种模式；同时运行时，端口、管理器 Data 和各项目 Data 必须独立。示例服务使用 8766，普通管理器默认 8765。同一个 Data 已被运行实例锁定时，第二个实例直接拒绝，不接管或结束原进程。原 --manager / --manage 入口保留；单独关闭旧 --manage 窗口仍不停止管理器。

## 准备私有启动配置

先按 [项目配置](PROJECT_CONFIGURATION.md)建立项目注册配置，准备已存在的工作目录，并在当前终端提供每个启用项目和所有者的凭据。启动配置会捕获全部启用项目凭据，即使只自动启动其中一部分。凭据须独立且符合现有格式。

以下为托盘示例，路径应替换成自己的实际位置，输出父目录须存在：

```powershell
$bot = 'D:/ShellBot/WinShellBot.exe'
$env:SHELLBOT_EXAMPLE_TOKEN = (& $bot --generate-token)
$env:SHELLBOT_OWNER_TOKEN = (& $bot --generate-token)
& $bot --startup-prepare --mode tray --projects 'D:/ShellBot/projects.local.json' `
  --manager-data-dir 'D:/ShellBotProjects/manager/state' `
  --output 'D:/ShellBotProfiles/tray.json' --start-project example
& $bot --startup-inspect --startup-profile 'D:/ShellBotProfiles/tray.json'
```

--output 必须是新文件，不覆盖旧配置。可重复 --start-project 选择多个已启用项目；省略时只启动管理器和网关，不启动项目。未知、重复或停用项目拒绝。prepare 不创建运行 Data、不登记自动启动、不启动主机；inspect 返回启动模式、项目选择、Data 和准确的项目配置快照，只返回凭据引用，不返回凭据值。

启动文件包含经过 Windows DPAPI 加密的完整项目快照和凭据。托盘绑定当前账户/电脑；服务使用 machine 范围，并依赖只允许文件所有者、SYSTEM 和该服务身份读取的保护 ACL。公开或继承的宽松文件 ACL 会拒绝加载。machine DPAPI 的安全性不能脱离文件 ACL，见 [Microsoft DPAPI 说明](https://learn.microsoft.com/en-us/windows/desktop/api/Dpapi/nf-dpapi-cryptprotectdata)。源配置修改和终端凭据轮换不会改变已有快照；修改后创建新的启动文件，检查后明确切换登记。启动文件不能放入任何授权工作目录、额外目录或运行 Data。它属于本机私有配置，应和运行 Data 一起保留，不能纳入 Git 或分发包。

完整解密设置最多 32 KiB，启动文件最多 64 KiB。传入的项目格式、权限和物理范围仍使用原校验。准备时所需的凭据只写入密文；运行后仅提供给本实例，工作进程继续排除凭据环境变量，不修改系统或当前用户的永久环境变量。具有任意同账户或管理员代码执行能力的程序仍属于原管理信任范围。

## 托盘与登录启动

保留同目录的 WinShellBot.exe 与新产品组件 ShellBot-Launcher.exe；PDF 和受限后端仍分别需要原 pdfium.dll、ShellBot-SandboxProbe.exe。启动器是原生 C17 Windows GUI 程序，仅启动相邻主程序的 resident 角色，不执行脚本或请求提权。

```powershell
& 'D:/ShellBot/ShellBot-Launcher.exe' --startup-profile 'D:/ShellBotProfiles/tray.json'
# 登记、查询和关闭本配置的登录启动：
& $bot --logon-enable --startup-profile 'D:/ShellBotProfiles/tray.json'
& $bot --logon-status --startup-profile 'D:/ShellBotProfiles/tray.json'
& $bot --logon-disable --startup-profile 'D:/ShellBotProfiles/tray.json'
```

启动器正常返回只代表已创建主程序，是否真正运行要检查托盘或 --management-status。resident 在自身进程内启动管理器，凭据不依赖登录时的终端环境。成功登记图标后默认隐藏管理窗口；图标注册失败时显示窗口以便管理，并在定时器和 TaskbarCreated 通知中重试。单击图标打开管理，右键可隐藏、设置登录启动或退出。关闭管理窗口只隐藏，后台项目和任务继续；右键“退出 ShellBot（停止本实例）”或原 --manager-stop 停止本管理器及任务，其他实例保留。

登录启动使用当前用户的 Run 项，名称按规范启动文件路径摘要生成，仅登记带完整引号的相邻启动器和启动文件路径。重复登记同一命令不会重复创建项目；关闭仅移除准确匹配的本配置项，其他项和被改成不同命令的同名项保留。即使启动文件已删除，也可用同一原路径查询/关闭原命令。移动程序或启动文件前先关闭旧登记。Windows 允许延后 Run 程序执行，命令最多 260 字符，见 [Microsoft Run 说明](https://learn.microsoft.com/en-us/windows/win32/setupapi/run-and-runonce-registry-keys)。

设置操作在后台处理；真正的 Run 登记写入由同一产品的独立、受 Job 管理的辅助进程完成，最多等待 20 秒。Windows 调用没有完成时，返回“状态未确认”，关闭本次辅助进程，不自动再次写入；先用 --logon-status 核对实际状态后再决定。2026 年 10 月 12 日用户配合后，登记、重复登记、不同命令保护和移除恢复专项已通过；用户报告允许了两次安全软件提示，具体提示内容未知。此前等待的根因仍未确定。

## 管理员启动与无人值守

以管理员身份打开 ShellBot-Launcher.exe，或在已提升终端运行 --startup-console / --manager 时，管理器使用已经获得的管理员令牌创建共享代理，不再次调用 runas。配置中有启用且允许管理员的项目时，自动创建一次代理；停止后不会自行恢复，所有者可显式重新启动，仍复用当前管理器权限。普通启动的管理器继续由所有者明确启动代理并确认 UAC。

项目仍须配置 execution.administrator_allowed:true，工具调用仍须明确 identity:administrator，并满足工具开关、文件范围和原有审批规则。普通任务不会因管理器提升而变成管理员任务；工作进程使用同账户、同会话的实际普通令牌，restricted 子任务继续使用 LPAC。无法取得并核验普通令牌时拒绝启动，不以管理员令牌回退。Windows 只提供身份查询级关联令牌或没有关联令牌时，改为核验当前登录 shell 的普通主令牌；等待 shell 就绪最多十秒，仍不可用则提供错误。此路径不支持跨账户提权或没有普通登录身份的环境。

需要以后登录也可无人确认 UAC 时，在首次已提升的所有者终端登记：

```powershell
& $bot --admin-logon-enable --startup-profile 'D:/ShellBotProfiles/tray.json'
& $bot --admin-logon-status --startup-profile 'D:/ShellBotProfiles/tray.json'
# 关闭也须使用已提升的所有者终端；不删除配置或 Data：
& $bot --admin-logon-disable --startup-profile 'D:/ShellBotProfiles/tray.json'
```

托盘提供“登录时以管理员身份自动启动”和“关闭管理员登录自动启动”；仅已提升的托盘可用。查询无需提升。设置命令本身不会请求 UAC。普通 Run 和最高权限计划任务对同一启动快照互斥，切换前先明确关闭原登记。

原生 Task Scheduler 接口登记当前 SID 的交互登录触发器和 Highest 权限，准确绑定相邻 GUI 启动器、启动文件参数及工作目录。只在该账户登录后运行，不提供登录前的桌面，也不保存 Windows 密码。设置为忽略重复实例、无执行时长上限、允许电池运行，由 Windows 在失败后间隔一分钟重启，最多三次；重启只启动新实例及配置选定项目，不重放历史任务或批准。登记后回读实际定义和启用状态；移除后回读确认不存在。同名任务的账户、动作、触发器或受管设置不匹配时拒绝覆盖或删除。移动前先关闭旧登记。权限规则见 [Microsoft Task Scheduler 安全上下文](https://learn.microsoft.com/en-us/windows/win32/taskschd/security-contexts-for-running-tasks)。

本保证覆盖 ShellBot 自身的管理员代理与任务路径；另一个程序主动请求 runas、跨账户认证、Windows 安全桌面及其自身认证仍遵守 Windows 规则。桌面工作进程保持普通权限，不因此允许输入管理员窗口或 UAC 安全桌面。

## 开机服务

参考 [service.example.json](../config/service.example.json)准备独立注册配置。示例为本地配置范围、只读、逐次审批，命令关闭；源工作目录须存在。先在普通终端捕获服务项目和所有者凭据并检查快照：

```powershell
$env:SHELLBOT_SERVICE_EXAMPLE_TOKEN = (& $bot --generate-token)
$env:SHELLBOT_SERVICE_OWNER_TOKEN = (& $bot --generate-token)
& $bot --startup-prepare --mode service --projects 'D:/ShellBot/service.local.json' `
  --manager-data-dir 'C:/ProgramData/ShellBotService/Manager' `
  --output 'D:/ShellBotProfiles/service.json' --start-project example
& $bot --startup-inspect --startup-profile 'D:/ShellBotProfiles/service.json'
```

在所有者明确打开的管理员终端中安装；程序不会隐式弹 UAC。安装权限由 Windows SCM 控制，见 [服务访问权限](https://learn.microsoft.com/en-us/windows/win32/services/service-security-and-access-rights)。只操作固定 ShellBotService，校验主程序、启动文件路径及虚拟账户绑定；同名其他服务拒绝修改或删除。

```powershell
# 管理员终端；C:/ProgramData/ShellBotService 须预先存在
& $bot --service-install --startup-profile 'D:/ShellBotProfiles/service.json'
& $bot --service-start --startup-profile 'D:/ShellBotProfiles/service.json'
& $bot --service-status --startup-profile 'D:/ShellBotProfiles/service.json'
```

安装创建自动（延迟）启动的非交互服务，使用独立虚拟账户，不保存 Windows 账户密码，不使用 LocalSystem 作为工具身份。只为该账户增加本启动文件的读取许可，创建新的私有管理器/项目 Data，或接受上次已授予该服务的私有 Data；不改动无关已有 Data 的 ACL，不扩大工作目录权限，不自动启动服务。立即父目录须已存在。安装失败时只撤销本次新建的服务对象，保留已创建的私有目录以供核对，不递归清理用户目录。

**还须由所有者为实际工作目录和必要程序授予 NT SERVICE\ShellBotService 相应权限。** 只读目录授予读取；需要上传的明确可写目录授予必要修改权限，同时设置 read_only:false 并启用 artifact_upload。不要授予整盘；不能借安装服务扩大原项目范围。服务可能因目录、程序或 Data 权限不足而启动失败。--service-start 的回执只代表请求已提交，--service-status 显示 running 才代表控制器确认服务运行；仍须实际调用项目验证工具。

service-run 仅接受准确的虚拟服务 SID、Session 0、无已启用管理员组且非受限/非 AppContainer 的父令牌，由 SCM 进入 ServiceMain。Windows 实测该虚拟账户的 TokenElevation 为真，但管理员组未启用；诊断如实保留提升标志，不把它当作管理员权限。服务不会创建管理员代理。交互管理器可接受已提升的所有者，普通工作进程和受限子任务仍独立核验普通及 LPAC 令牌。

START_PENDING / RUNNING / STOP_PENDING / STOPPED 分别报告，停止/系统关闭只发出停止信号，再清理本人工作进程和任务；清理异常超过等待预算时结束本服务进程，由原 Job 收尾。不配置自动失败重启，不重放旧任务。某个自动启动项目失败时，其 failed 状态可检查，其他已启动项目继续；不会退回更高权限。

服务若启用 restricted 命令，还须在 execution.runtime_files 列出准确程序，并为服务账户授予工作目录、相邻 ShellBot-SandboxProbe.exe 和这些程序的 ACL 初始化与恢复权限。仅普通“修改”权限不足以调整 DACL。建议使用专用服务程序副本和专用目录，按这些准确对象授权；不能授予整盘或无关开发目录。未满足条件时命令后端保持关闭，文件工具仍遵守原策略。

所有者审批使用该服务端口的 /owner 页面及捕获的独立所有者凭据。普通登录账户的原本机管理管道不跨服务 SID 接管服务管理器，--manage 用于普通实例；服务生命周期使用 SCM 命令。若同机还需要桌面功能，使用普通登录会话中的独立管理实例，桌面项目统一由该管理程序的托盘控制。

```powershell
& $bot --service-stop --startup-profile 'D:/ShellBotProfiles/service.json'
# 确认 stopped 后才能卸载；保留启动文件、Data 和历史：
& $bot --service-uninstall --startup-profile 'D:/ShellBotProfiles/service.json'
```

卸载不删除文件或自动重置已明确授予的服务文件权限。更换程序位置或启动配置要先停止/卸载原绑定，再按新快照安装；保留旧程序、配置和 Data，不能用新配置接管别人的同名服务。

## 验证边界

2026-10-17 用户配合完成此前版本的真实 UAC 安全桌面拒绝、旧租约失效以及实际管理员窗口输入/切换拒绝，普通窗口正向对照通过；原报告保留原程序摘要。随后管理员启动专项 307 项通过，实际覆盖提升管理器、普通与 LPAC 任务、两个项目共享管理员、审批和许可拒绝，以及 Windows 最高权限计划任务的实际登记、按需启动和移除。没有重启，按用户新范围不要求实际重启/登录验收，详情见 [人工验收记录](MANUAL_ACCEPTANCE_20261012.md)。

本机已验证私有配置/密文、原配置冻结、凭据环境缺失时实际 HTTP 工具、真实托盘注册回执/窗口显隐、后台原生任务继续与退出清理、相邻启动器、重复 Data 拒绝、服务策略/身份/普通安装拒绝及服务策略下的前台诊断。TaskbarCreated 通过本窗口消息验证恢复逻辑，没有重启真实 Explorer。--startup-console 可前台运行同一启动配置作诊断，不代表 SCM、虚拟服务账户或未登录开机已验收。

随后人工重启真实资源管理器，发现桌面控制图标未恢复，已补桌面窗口的 TaskbarCreated 处理与两秒重试；修复后用户确认项目管理与桌面控制两个图标及菜单均恢复。重复消息只更新本图标，不触发恢复控制或重放动作。当前环境无法结束 Explorer，完全无任务栏启动的备用窗口路径仍待独立验收。

2026 年 10 月 12 日实际 SCM 与虚拟账户专项已验证独立测试服务的运行、Session 0、只读下载、授权上传、独立所有者审批、真实 LPAC 子任务及普通账户本机管理拒绝；物理托盘显隐、退出、停止热键及真实 Explorer 重启后的图标恢复也已取得用户证据。历史记录不改写为新程序的全面人工重跑。测试脚本继续仅在开发机本地保留；常规开关为 -Startup，可用 -SkipLogonRegistration 明确跳过登记专项，报告标注 excluded_pending_environment。
