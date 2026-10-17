# ShellBot 2026-10-12 人工验收

## 2026-10-17 最新：管理员启动、Windows 登记与清理完成

用户明确调整范围：自启只要求按 Windows 规范正确登记启动项、计划任务或服务并回读核对，不要求真实重启或注销。下方原“等待重启”、登录界面等待三分钟和 boot_acceptance.py 步骤均作为历史保留，本轮不再执行，也不标为已通过。Windows 负责触发，应用路径、权限、配置和依赖失败仍须提供诊断。

本轮实现已提升交互管理器复用已有令牌启动共享代理；普通工作进程使用实际普通令牌，LPAC 后端和项目许可、审批维持原边界。新增当前账户的最高权限交互登录计划任务，不保存 Windows 密码；普通 Run 与该任务对同一启动快照互斥。Windows 管理失败后的最多三次、间隔一分钟重启，不恢复旧批准或重放任务。服务保持独立虚拟账户与自动延迟启动。

当前 Release SHA-256 为 c311e61ddefa3d49e4484227e9c31de99cfbcffc74580c5edee2ab4d1ef6a629，Debug 为 d14fa968f5c30ee4812747d9529e06efa9b63d8284bb12c086d792443c7bd31c。Debug / Release 各 14 份常规报告绑定这些摘要，分别通过 7062 / 7068 项，包括各 390 项桌面回归及 124 项启动检查。用户配合停手，桌面回归结束后已通知恢复操作，剪贴板恢复成功。历史 HTTP close 专项仍明确排除，计数包括轮询与文件扫描。

- Release 管理员启动专项 307 项通过：实际管理员管理器、五个普通 worker、两个获准项目的真实管理员任务、普通与 LPAC 任务、配置审批及禁止管理员项目的拒绝；显式停止/重启代理复用当前管理器权限。实际 Windows 计划任务登记、独立 XML 回读、按需启动最高权限托盘、再次完成相同任务检查及准确移除均通过。未重启。原始报告 build/validation/x64-Release-m12-elevated-startup-manual-20261017.json，夹具目录 build/elevated-startup-tests/20261017-150901-eed1c463。
- 用户对最后成功一轮的 UAC 次数回答“没留意，无法确认”。自动化未观察屏幕弹窗次数；证据确认现有令牌复用与实际任务成功，不能改写为用户确认“仅一次”。前面开发复测的 UAC 不与最后成功轮混算。
- 计划任务归属专项 10 项通过：实际把本轮新建测试任务的动作参数改为 --version，查询显示未归属，启用拒绝覆盖、关闭拒绝删除，实际 XML 均保持被改后的原样；恢复准确原定义后可正常移除。报告 build/validation/m12-task-ownership-manual-20261017.json。
- 清理前由当前程序实际查询 Windows 服务配置，确认 automatic_start / delayed_start / virtual_service_account 均为真，另由旧准确二进制确认服务绑定。之后停止、卸载专用 ShellBotService，移除准确测试 Run 值；13 项独立清理核对通过，全部无关 Run 值的类型和内容摘要不变。五个工作目录/启动文件/运行文件 DACL 与原快照逐字节一致。旧配置、Data、历史、程序和报告保留。收据 build/validation/m12-system-registration-cleanup-20261017.json、m12-acl-native-restore-20261017.json。

恢复过程中发现旧 PowerShell 清理脚本的 Set-Acl 会重建继承并重复部分 ACE，初次清理核对如实失败并保留；已改用 Windows 原生 SetFileSecurityW 在准确五个对象上恢复，独立逐字节核对通过，本地清理脚本同步修正。未递归删除目录，也未改变实际用户项目、Data 或隧道。

保留首次完整回归的 M2 失败和同程序独立复测通过报告；首轮具体底层错误未捕获，不能称为根因已修复。管理员启动开发期间的测试配置错误、Windows 关联令牌 1346、进程创建 87/5/1314 以及 schtasks 中文输出解码失败均保留各自原始目录/报告；最终程序使用经同 SID/会话校验的普通主令牌和核对实际子进程的私有通道，最终完整回归及真实专项通过。

本轮总收据为 build/validation/m12-elevated-startup-acceptance-20261017.json。153 个源码/文档文件及 48 个本地测试/脚本分别有清单；开始前的 152 文件、两套旧程序和旧报告保留在 build/baseline-dev19-elevated-startup-20261017-144411，旧 11 个 ZIP 保留。仍为 main / 051bf77，本地修改未提交、未推送，未制作新包。独立系统、真实外部客户端/另一设备、RDP、多屏缩放及无 Explorer 初始备用窗口仍按原范围另行验收。

以下是此前各轮的原始进度与历史验收步骤，不代表当前源码摘要、当前系统登记状态或仍要求重启。

## 2026-10-17：桌面安全边界验证完成

用户明确要求先做桌面安全边界验证并配合 UAC。本次仓库仍为 main / 051bf77，产品源码和程序未改变；Release SHA-256 仍为 dc21316abe6148814520bbfaa2a6e387cec7068bfb1b50521a42fe499bff87f2，Debug 仍为 3bbcb1b8f27250483542981cf5bbf7330c8bb6dbf43f82ae1759e7c8976c3681。本次只运行 Release 人工专项，不能写成 Debug 也完成了此次人工验证。

- 真实 UAC 安全桌面：独立 OpenInputDesktop 查询测得不可访问，产品同时报告桌面不可用；新控制申请明确返回 no_desktop_session，原租约下的观察、截图及输入均拒绝且没有图片载荷。返回后旧租约依然拒绝，测试服务的已认证 HTTP 仍响应。用户确认“屏幕变暗，保持约 15 秒后点击了‘是’”；15 秒为用户估计，不称为自动测量。
- 实际管理员窗口：原生夹具自行核对同账户、同会话、TokenElevation:true / 完整性 12288；桌面 worker 的实际完整性为 8192、未提升。文字、按键、点击、拖动和滚动五类请求均以 input_not_permitted 拒绝，使用新观察、有效租约，排除了暂停或旧观察导致的假通过。窗口切换到管理员目标也按相同权限规则拒绝。
- 独立效果对照：管理员 Edit 的 26 字符测试文本未变，字符/按键/鼠标按下/抬起/滚轮计数均未增加；专用普通窗口实际收到 6 个对照字符。未读写剪贴板，剪贴板序号不变。没有向其他应用输入。
- 本轮 manager、worker、普通和管理员夹具均已退出，租约已释放；旧 ShellBotService 及准确测试登录项继续保留给后续开机验收。本次没有注销、重启或改变 UAC/安全软件设置。

证据分为两次实际运行，不合并伪造单次通过：build/validation/x64-Release-m8-security-boundary-manual-third-20261017.json 的安全桌面及旧租约检查已通过（52 条通过记录），但整轮因随后 Windows 拒绝夹具前台切换而保留 passed:false。build/validation/x64-Release-m8-elevated-window-manual-third-20261017.json 独立完成管理员窗口与普通窗口对照，78 条检查、passed:true。聚合收据 build/validation/m8-desktop-security-boundary-20261017.json 按上述明确子项引用两份原始报告；用户观察另存 m8-boundary-human-observation-20261017.json。

新原生 DesktopBoundaryFixture 和本地 Python 专项只属于开发机测试，仍被 Git 忽略，不是产品依赖。早期夹具换行问题、Windows 前台激活限制、人工输入造成的暂停和一次静止等待不足均保留原失败报告。用户未操作第二次 UAC，Windows 返回 1223；不记为用户点击了“否”。后来重新弹出并实际允许，完成管理员窗口专项。只有需要用户选中专用窗口时才等待人工，激活后重新取得租约/观察，不修改 Windows 前台锁设置或绕过权限。全部旧收据与开机前记录保持原样。

本次开始时读到系统运行会话已变化、准确测试服务仍运行、旧托盘管理入口不可用；这不代替此前约定的“登录界面等待至少三分钟后再登录”的实际开机/登录验收。重启/登录自动启动、最终服务卸载和原 ACL/测试登录项恢复仍待后续明确进行；外部客户端、另一设备审批/代理、不同缩放多屏、RDP、独立系统和无任务栏初始窗口继续按原范围待验。

## 历史进度：原等待重启要求已被最新范围取消

最新用户回复“暂时不能重启”。本次停在已准备好的重启验收点，未执行重启或注销；开机前登录服务与实际登录自动启动均仍待验证。重启交接收据为 build/validation/m12-reboot-ready-20261012.json，暂缓记录为 build/validation/m12-reboot-deferred-20261012.json。准确测试服务继续运行、测试登录项保留，托盘测试实例已停止；用户方便时按下文步骤手动重启，再从 after 验证继续，不重新覆盖 boot-before-latest.json。

用户已回复“现在可以继续人工验证了”，上一节暂停约束已解除。仓库仍为 main / 051bf77，M12 修改未提交、未推送。服务与测试登录项均核对为准确绑定。

服务重启后的旧任务/历史检查已通过 4 项：新 worker 实例、原任务 inactive 历史、旧实例查询拒绝、将原 request_key 带入新实例也不能申请或执行。没有重放旧任务。

用户实际重启资源管理器后发现仅项目管理图标恢复，桌面控制图标丢失。已为 desktop.c 补 TaskbarCreated 重新登记、重复通知时 NIM_MODIFY 和失败后的两秒重试，不改变桌面租约、暂停、恢复或输入规则。修复前源码/两套 out 及旧人工收据保留在 build/manual-before-desktop-tray-recovery-20261012。修复后 Debug / Release 桌面各 390 项通过，包含真正移除本测试 icon 再通过窗口回调重新登记；两套 13 份普通回归也通过。

用户再次真实重启资源管理器，确认两个图标和桌面菜单均恢复；实际窗口属性和同一项目实例记录在 physical-before-explorer-fixed-*.json / physical-after-explorer-fixed-*.json。用户报告当前任务管理器不允许结束 explorer.exe，未强行终止或改变 Windows 设置；完全没有任务栏时的初始备用窗口仍未实际验收。

最新 Release SHA-256 dc21316abe6148814520bbfaa2a6e387cec7068bfb1b50521a42fe499bff87f2；Debug 3bbcb1b8f27250483542981cf5bbf7330c8bb6dbf43f82ae1759e7c8976c3681。独立 service-bin 已保存旧程序并更新到同一 Release 摘要；实际 SCM 文件/上传/LPAC 10 项与防重放 4 项再次通过。下方 55f4082a 的管理员记录属于修复前程序；这些实际报告保持原摘要，不冒充最新程序的完整管理员重跑。此次产品源码仅桌面图标登记/恢复发生变化，管理员、服务身份、审批与传输代码未变。

重启前记录 boot-before-latest.json / boot-before-*.json 已通过 6 项，包含准确服务与登录项、真实 Session 0 身份、Windows 登录 FILETIME、当前系统运行时间和程序摘要。服务端口 59605；托盘端口 59607，目前托盘实例已停止，测试登录项仍启用。没有远程执行注销或重启，也没有添加开机探针任务或存储 Windows 密码。

实际重启步骤：保存当前工作，手动选择“重启”；到登录界面后先不要登录，等待至少三分钟，再登录同一 Windows 账户并返回此聊天。服务采用自动延迟启动，需要等它完成。回来后先运行本地 tests/boot_acceptance.py after --manifest build/m12-manual/20261012-121428-d8bfbd7b/manifest.json，再用 manual_acceptance.py observe / service-no-replay 核对托盘与历史。不能手动启动管理器/服务后再声称它们是自动启动。

未登录证据取各 service host_info.uptime_ms 与查询前后的 Windows FILETIME，计算保守启动时间范围，要求最晚边界早于同一会话的实际 LogonTime；Gateway 的初始化在启动选定 worker 前完成。真实 HTTP 请求发生在登录后，将单独注明，不把它标成登录前的客户端调用。会话时间字段定义见 [Microsoft WTSINFOEX_LEVEL1_W](https://learn.microsoft.com/en-us/windows/win32/api/wtsapi32/ns-wtsapi32-wtsinfoex_level1_w)。时间或启动条件不满足时保留失败证据，不改成已通过。

全部验收后按准确 manifest 运行 service_manual_admin.ps1 -Phase Cleanup（需要用户允许 UAC），停止/卸载测试服务、恢复工作目录/启动配置/两份 runtime 文件原 ACL；--logon-disable 仅移除本测试配置准确匹配的登录项。保留加密配置、Data、旧程序和报告，不递归删除目录。旧历史记录、独立电脑、真实外部 MCP 客户端、RDP/多屏、历史 HTTP close 与硬链接条件仍按原范围分别保留。

以下是前一轮暂停时的历史记录，不代表当前程序摘要和人工可用性。

仓库 E:\Program\ShellBot，main，基准提交 051bf77a87243ec7ff6bfe4ac3d69d77acb247c2，dev.19 未提交、未推送、未打包。用户授权补齐此前人工验证，当前仅完成本电脑；独立系统、RDP、多屏和真实外部 MCP 客户端仍待对应环境。

## 服务身份修复

实际 SCM 首次启动失败，System 日志 7000/7009 保留。短暂 ShellBotIdentityProbe 虚拟服务测得 Session 0、TokenElevation:true、administrator_enabled:false、restricted:false，随后已删除。合法虚拟服务令牌并非普通登录用户的 UAC 过滤令牌，不能仅凭 TokenElevation 拒绝。

新增准确 ShellBotService SID/Session 0/无已启用管理员组且非受限或 AppContainer 的准入核验，用于 service-run、服务管理器、项目 worker 及受限任务初始化。普通登录用户入口保留原未提升要求，旧管理员入口不放宽。管理器如实报告父令牌提升标志；实际受限子任务仍独立验证未提升 LPAC，任务回执报告子身份。

旧 151 源码文件、原 out 和原验证报告保留于 build/manual-before-service-token-fix-20261012。服务第二版使用独立 service-bin 程序副本，只对测试工作目录和两份原生夹具明确授予 ACL 初始化权限，保存原 DACL；不授予实际开发目录或整盘。

最终 Release SHA-256 55f4082ace0ab8710e4b228d591efd3aa1edf2d986aa31e5331e3f8bc481e94f；Debug 44420f19ba14503910920433a17ae4d70a01f0cf9ac7a412520aa29f07b157cb。旧 e9c088f7 程序下的人工报告不冒充最终版本。

## 当前证据

- 修复后 Debug / Release 常规回归各 13 份通过，合计 6698 / 6699 项；Release M12 含实际登录登记共 124 项，Debug 明确跳过登记共 105 项。
- 最终 Release 完整共享管理员专项 470 项；取消启动第二轮 132 项通过。取消第一轮测得代理已启动，用户确认误点“是”，原失败报告保留。
- 最终 Release 管理员审批/恢复与本地 Edge 页面 513 项、管理窗口 113 项、保护图像解码/缩放 876 项、文档 356 项、传输 1091 项、管理器退出清理 152 项，均匹配最终 Release 摘要。计数包含轮询和往返，不是功能数。
- 最终 Release 桌面自动专项 383 项通过。实际专用控件输入、截图、停止事件、剪贴板恢复不冒充物理停止和托盘操作。
- 实际 SCM 运行专项 10 项通过：准确虚拟 SID、Session 0、父 TokenElevation:true/管理员组关闭、独立 owner 审批、只读下载、授权上传与字节/摘要复核、真实未提升 AppContainer/LPAC 任务、普通账户不能接管本机服务管理管道。
- 实际服务启动一个受限等待任务后，停止核对原服务、两个 worker 和等待任务共四个 PID 全部退出，再显式启动成功。service-admin-Restart-20261012-125714-236.json / service-live-before-stop.json 保留证据；服务重启后的旧 task key/历史不重放检查尚未执行。
- 物理托盘单击打开（实际 HWND 可见）、关闭隐藏且项目继续、Ctrl+Alt+F9 停止（实测 emergency_stop_count:1）、桌面托盘确认恢复及项目托盘退出（管理器/worker 退出、服务保留）通过。首次恢复时仍暂停；释放租约后再次确认且等待一秒，实际 paused:false，审计含 local-tray/desktop_resume_requested 与 desktop_resumed。产品恢复要求约 500 ms 无输入。
- 实际锁屏/解锁观察通过：用户报告等待约 15 秒；工具记录 48 个样本，其中 1 个 desktop_available:false，同时服务 HTTP 响应成功，锁屏桌面请求及解锁后旧租约均被拒绝。15 秒是用户观察，不称为自动测量。重启前登录测试项最初再次超时且状态不存在，用户从托盘允许登记后核对 logon_enabled:true / owned_entry:true。
- 登录登记先前超时的根因未确定；本轮登记、重复登记、不同命令保护及完整移除恢复通过。用户观察两次安全软件提示，显示 ShellBot.exe 和 python.exe，均点允许；具体内容未知。未更改安全软件、UAC 或系统执行策略。

首轮管理员传输已完成保护源下载和保护目标上传，但普通清理进程不能重置管理员创建的新文件权限。通过准确单文件 UAC 恢复，并修正本地夹具复用原管理员任务后端清理，重跑通过。管理窗口首轮在异步确认后的清理检查失败，补确认窗口销毁/实际进程退出等待，原报告保留。新增服务脚本首轮命名遮蔽及缺少 runtime_files 的夹具错误均保留，修正后真实服务/LPAC 通过。新增管理员图像夹具首轮解码返回值解包错误，权限/进程已恢复，原 721 项失败报告保留，修正辅助代码后 876 项通过。

## 暂停点与恢复

用户最新要求：“请在合适的位置暂停，我要离开一会，将暂时无法继续人工验证”。已暂停，不再弹 UAC、执行锁屏或安排重启。无需人工的 --manager-stop 已停止普通托盘实例；没有控制租约或自动桌面动作。准确测试服务及测试登录项暂留，尚未做最终清理；下一轮须待用户明确回来后继续需要配合的操作。

独立人工夹具与加密启动配置在 build/m12-manual/20261012-121428-d8bfbd7b，manifest.json 不含明文令牌。当前 ShellBotService 暂留安装用于后续开机验收，绑定该目录 service-bin/WinShellBot.exe 与 service-v2.json，监听本机 59605；托盘配置使用独立端口 59607 和独立 Data。没有自动重启、注销或变更真实用户项目/隧道。

物理托盘点击、紧急停止/人工恢复、锁屏已完成；实际登录、未登录开机、最终版本服务卸载/ACL 恢复，以及新服务实例旧任务/历史不重放核对尚未完成。实际 Explorer 重启与图标不可用场景也未执行。开机测试前应保存当前工作，由用户明确执行重启，不自动注销/重启。未登录服务应取得登录前启动或 HTTP 响应证据，不能仅以登录后的 RUNNING 冒充。

验证完成后停止/卸载准确绑定的测试服务、移除准确匹配的测试登录项并恢复原 ACL，保留加密配置、Data、报告和旧程序。管理员恢复脚本为本地 tests/service_manual_admin.ps1，支持准确 manifest 下的 Cleanup，不递归删除目录。常规与管理员报告位于 build/validation，实际服务报告位于上述独立夹具目录。停止托盘不会移除已登记的测试登录项，下次登录仍可能启动该测试实例。

保留的测试服务绑定：E:\Program\ShellBot\build\m12-manual\20261012-121428-d8bfbd7b\service-bin\WinShellBot.exe --service-run --startup-profile E:\Program\ShellBot\build\m12-manual\20261012-121428-d8bfbd7b\service-v2.json。当前状态 RUNNING，服务 PID 12048；新 PID 属瞬时状态，恢复时重新核对。测试登录项启动 out/x64/Release/ShellBot-Launcher.exe 和同目录夹具 tray.json，仅该准确绑定项可清理。实际用户配置、Data、隧道保留。

后续启动先读取此文件、manifest.json 与最新 service-admin-*.json，确认服务与程序摘要；不把未登录开机或物理点击提前标为通过，不重放历史任务。
