# Windows Server 2025 独立运行测试

这是 ShellBot `0.1.0-dev.7` 功能测试包，新增普通/管理员后台命令及原生任务专项。原有覆盖、局部替换、私有备份和完整文本摘要保留；完整受控隔离、规则和远程审批尚未实现；管理员命令在 Windows 11 本机已验证，Server 2025 需新版本独立回归。程序仍只监听本地端口，不要求检查真实域名或管理隧道。第一阶段仍在开发；解压到新目录并保留旧包和结果。

## 运行方法

将整个 ZIP 复制到测试电脑，解压到有写入权限的目录。无需安装 Visual Studio、Node.js、Python 或 VC 运行库。常规测试不需要管理员权限；管理员代理专项测试显式要求启动授权。Windows Server Core 可测试常规能力，真实 UAC 交互须使用具备相应交互与策略条件的会话，不能从常规通过推断此项通过。

在解压后包含 `WinShellBot.exe` 的目录打开 PowerShell，执行：

```powershell
powershell.exe -NoProfile -File .\Test-ShellBot.ps1
```

若系统策略不允许运行本地测试脚本，应按所有者已有管理策略处理；程序不自动修改策略。脚本不安装服务、不调整防火墙、不修改系统配置，HTTP 只监听随机的 `127.0.0.1` 端口。文件操作全部在包目录下新建的 `test-results/时间-随机标识` 内进行，不操作你的现有工作目录。

测试包括：版本、十三个工具、多目录、Unicode、内容搜索、图像、路径限制、本机审批、默认禁止覆盖、绑定哈希的显式覆盖与局部替换、完整摘要、原文备份和私有 ACL、审批后内容冲突拒绝、重复请求、审计和重启行为，以及填写和未填写域名时的本机 HTTP 请求头、令牌和停止。所有自动批准只涉及本次隔离测试目录。

此外会各执行 100 次 fresh keep-alive 和 `Connection: close` 调用，不重试失败请求。开发机 Windows build 26300 已在 ShellBot 和独立参考服务中复现连接重置，尚未确定根因。关闭连接和对照组均通过才会标记整体通过。

结束时会显示报告目录，其中 `report.json` 记录系统版本、可执行文件 SHA-256、各项检查和关闭连接的失败数量：

- `status: passed`：本次核心、基本 HTTP 与两组连接诊断均通过；-HttpOnly 时核心为 not_run。
- `status: needs_investigation`：基本检查通过，两组连接诊断中至少一组发生失败。
- `status: failed`：核心或基本 HTTP 检查失败，报告中包含失败信息。

后两种情况返回非零退出码。请把 `report.json` 发回，并说明测试电脑是 Server Core 还是桌面体验版本。测试文件保留，便于检查结果；可以在确认程序全部退出后手动删除本次 `test-results` 子目录。

脚本中的 shellbot.example.test 和 forwarded.example.test 只用于本机模拟代理，不进行 DNS 或公网调用，也不要求用户检查已有隧道。图像是否实际到达模型仍需客户端使用证据。已有代理说明见 EXTERNAL_ACCESS.md，文本修改行为及保存限制见 FILE_EDIT.md。

2026 年 10 月 6 日收到 dev.2 的三次控制台反馈：首次核心通过、关闭连接 2/100 失败；15:08 原脚本复测核心通过、关闭连接 0/100 失败；15:09 新诊断整体通过、关闭连接及新建 keep-alive 对照各 0/100 失败。报告目录分别为 test-results/20261006-145240-8521abfb、20261006-150813-bb89ed55、20261006-150910-9ba8a44c。本次通过为继续开发提供独立机器证据；连接根因、具体系统 build 和程序 SHA-256 仍需完整报告核实，dev.3 的新增审计需另行验收。

dev.3 已收到两份完整报告并核验：Server 2025 Datacenter build 26100，两轮各 152 项核心和基本 HTTP 检查通过，包括审计 ACL、内容脱敏与重启保留；程序校验值匹配交付包。15:27:09 的 close 组 1/100 失败，失败发生于 response_headers、0 字节、10054；15:27:22 的 close 组 0/100，两轮 fresh keep-alive 均为 0/100。新增审计的基础场景已通过独立机器测试，连接问题仍待处理。完整证据与限定范围见 [实现与验证记录](IMPLEMENTATION_STATUS.md)。

dev.4 的 20261006-161731-6534f05c 完整报告已核验：Server 2025 Datacenter build 26100，160 项核心和基本 HTTP 检查通过，程序校验值匹配；close 和 fresh keep-alive 均完成 100 次，各 0 次失败。新增内容搜索及外部域名请求头检查已通过这一轮独立测试；实际已有隧道和 MCP 客户端仍另行验证，偶发连接重置保留未修复状态。

2026 年 10 月 7 日 dev.5 的 20261007-125434-ec039ee1 完整报告已核验：同一系统版本上，195 项核心与基本 HTTP 检查全部通过，程序 SHA-256 与交付包一致。新增覆盖、局部替换、完整摘要、原文备份与备份私有 ACL、审批后内容冲突拒绝，以及无需填写域名的代理请求头检查通过。close 完成 100 次、1 次失败，fresh keep-alive 完成 100 次、0 次失败，整体 needs_investigation；唯一失败为响应头收到 0 字节时发生 10054。功能通过与连接问题分别保留，不扩大为真实客户端、NTFS 多硬链接或故障恢复验收。原始报告已归档，详见 [实现与验证记录](IMPLEMENTATION_STATUS.md)。

## 权限基础层专项测试

在普通权限 PowerShell 中运行以下脚本；加 -Administrator 会显式请求管理员代理的启动授权：

```powershell
powershell.exe -NoProfile -File .\Test-ShellBot-Permissions.ps1 -Administrator
```

脚本检查三档配置、真实普通用户身份、受限调用者无法使用所有者管理入口，并在授权后连续查询同一个管理员代理 20 次，检查主机退出后代理退出。用户只需处理启动授权，不逐次授权状态查询。报告标明 skipped、主机是否已提升、代理身份及权限；脚本不自动观察 UAC 弹窗次数，若出现额外弹窗请随报告说明。取消启动授权会报告失败，不退回其他档位。

此脚本不运行任意命令、不创建低权限用户、不安装服务或计划任务、不改防火墙。它只验收权限身份与代理通信，不能替代命令执行、受控沙箱或远程审批验收。已有管理员权限的 PowerShell 会跳过普通用户及受限调用者场景，因此建议使用普通权限会话。

## 手动检查审批

准备一个空的测试目录，编辑 `shellbot.example.json`：填写绝对目录路径，并仅对需要测试写入的 root 设置 `read_only: false`。运行：

```powershell
$env:SHELLBOT_TOKEN = .\WinShellBot.exe --generate-token
.\WinShellBot.exe --http --config .\shellbot.example.json --data-dir C:\ShellBotTestData
```

在第二个同一 Windows 账户的终端中管理请求，数据目录必须与运行中的主机相同：

```powershell
.\WinShellBot.exe --list-approvals --data-dir C:\ShellBotTestData
.\WinShellBot.exe --inspect 审批ID --data-dir C:\ShellBotTestData
.\WinShellBot.exe --approve 审批ID --data-dir C:\ShellBotTestData
.\WinShellBot.exe --deny 审批ID --data-dir C:\ShellBotTestData
.\WinShellBot.exe --revoke 审批ID --data-dir C:\ShellBotTestData
.\WinShellBot.exe --stop --data-dir C:\ShellBotTestData
```

默认 fs_write 仍只创建；首次返回审批 ID，本机 --inspect 后 --approve，再以同样参数和 approval_id 重试。显式 overwrite:true 与 fs_patch 另外要求 expected_sha256，修改前保留私有备份，原文和结果均限 64 KiB UTF-8。锁定原文件写入，不承诺崩溃原子性；详见 FILE_EDIT.md。

审批默认 300 秒，配置可设置 `approval_ttl_seconds` 为 1～600 秒。最多保留 64 条内存记录，主机退出后全部失效。dev.3 把审批和执行事件持久保存为私有 JSONL，单实例默认 8 MiB；旧审批不恢复，也没有重启任务恢复，见 [审计说明](AUDIT.md)。程序异常中断可能留下其自行创建的 `.shellbot-随机值.tmp`，不自动按文件名前缀删除文件。

本地管理通道拒绝远程管道连接，检查 Windows 账户身份；同一账户下的其他本地程序属于信任范围。这不是区分同账户内“人”和“自动程序”的安全边界。controlled 开放命令前还需要完整隔离；user/administrator 是预先授权的账户级信任模式。

包中的 `manifest.json` 和 `SHA256SUMS.json` 标识构建来源、依赖和文件校验值。项目许可证的版权主体仍是模板，正式发行前需要补全。

## dev.6 验收与远程自动测试

2026 年 10 月 9 日 Server 2025 Datacenter build 26100 两份原始报告已核验，程序摘要匹配交付包。常规脚本核心与基本 HTTP 199 项通过，close 1/100 失败，对照 0/100；权限专项在普通权限主机下 100 项通过，实际启动并复用同账户管理员代理 20 次，主机退出后代理退出。用户观察确认 UAC 只在代理启动时出现一次，脚本本身不统计弹窗。跨账户提权、命令执行与完整隔离尚未验收。详见 IMPLEMENTATION_STATUS.md。

Windows 11 本机可运行相同脚本，Server 2025 保留为独立环境回归。用户配置 SSH 后可远程运行非交互核心和网络诊断并回收报告。SSH 登录后应先查询实际令牌；已提升会话下权限专项会跳过普通用户与受限调用者场景，不能替代普通桌面首次 UAC 验收。首次 UAC 和后续桌面工具仍需交互会话验证；普通 SSH 终端不能显示或点击安全桌面上的授权提示。当前版本不为 SSH 安装服务、计划任务或改变 UAC 策略。

相关平台依据：[Windows OpenSSH 配置](https://learn.microsoft.com/en-us/windows-server/administration/openssh/openssh-server-configuration)、[UAC 工作方式](https://learn.microsoft.com/en-us/windows/security/application-security/application-control/user-account-control/how-it-works)、[服务与交互会话](https://learn.microsoft.com/en-us/windows/win32/services/interactive-services)。

## dev.7 原生任务专项

测试包额外包含 ShellBot-TestFixture.exe 及对应 C 源码，只有身份查询、隔离目录追加、stdin 回显、原始字节输出和等待五种固定行为。测试不启动编码 PowerShell 子命令，不要求安装 Python；产品运行无需测试辅助程序。先运行普通权限专项，再在普通权限会话请求管理员启动授权：

```powershell
powershell.exe -NoProfile -File .\Test-ShellBot-Tasks.ps1
powershell.exe -NoProfile -File .\Test-ShellBot-Tasks.ps1 -Administrator
```

管理员专项在启动代理时需要 UAC，随后多条任务应复用同一代理。脚本检查主入口未提升、实际子进程权限、幂等文件副作用、输入及 EOF、超时、取消、主机退出后任务和代理退出。报告标记 native_test_executable，并包含主程序/辅助程序摘要。脚本不能统计弹窗或杀毒软件告警；请将这些实际观察与 report.json 一同记录。

2026-10-09 Windows 11 build 26300 的普通 89 项、管理员 94 项通过，用户在首次改用原生辅助程序后确认没有新的杀毒告警、UAC 仅一次。该证据不自动覆盖其他系统或安全软件。若 Server 2025 出现拦截，保留原始报告和告警信息，先查看源码与文件摘要，避免反复重试或自动更改杀毒设置。完整限制见 COMMAND_TASKS.md。
