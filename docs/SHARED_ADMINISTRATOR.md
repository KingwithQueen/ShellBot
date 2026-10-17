# 共享管理员代理（M5 / dev.12）

dev.14 的[本机管理窗口](PROJECT_MANAGEMENT_UI.md)复用以下异步启动/停止协议；只有明确启动管理员才请求 UAC。暂停项目只撤离并结束该项目的管理员任务，其他项目和代理继续运行；窗口停止代理影响全部管理员任务，普通任务继续运行。

普通启动时管理器和网关使用普通权限，所有者显式启动管理员代理，已经允许的项目共同复用。dev.19 另支持已提升的交互所有者启动管理器：有预先允许的项目时自动创建一次代理，复用已有管理员令牌，不再次请求 UAC；工作进程、普通任务及 LPAC 任务仍使用原默认身份。最高权限登录计划任务见 [常驻与自动启动](STARTUP.md)。项目 MCP 不能启动 UAC、注册项目、授权管理员使用或批准自己的操作。

## 所有者操作

在普通权限终端运行管理器，项目配置继续使用 format_version:1。管理员项目必须为 current_user 文件范围和 current_user 默认命令身份；restricted、目录隔离和旧兼容项目不能使用共享代理。命令还要求 execution.enabled 和工具开关允许；只读范围不允许任意账户或管理员命令绕过限制。

```powershell
WinShellBot.exe --manager --http --projects E:\ShellBot\projects.json --manager-data-dir E:\ShellBot\ManagerData
WinShellBot.exe --project-start A --manager-data-dir E:\ShellBot\ManagerData
WinShellBot.exe --project-start B --manager-data-dir E:\ShellBot\ManagerData
WinShellBot.exe --administrator-start --manager-data-dir E:\ShellBot\ManagerData
WinShellBot.exe --administrator-status --manager-data-dir E:\ShellBot\ManagerData
WinShellBot.exe --project-administrator-allow C --manager-data-dir E:\ShellBot\ManagerData
WinShellBot.exe --project-administrator-revoke C --manager-data-dir E:\ShellBot\ManagerData
WinShellBot.exe --administrator-stop --manager-data-dir E:\ShellBot\ManagerData
```

项目配置中的 execution.administrator_allowed 是预先保存的所有者许可。allow/revoke 是当前管理器运行期的显式更新，返回 authorization_version、persisted_to_registry:false；不会修改被固定的配置文件。永久更改继续使用 project-configure 导出新配置，并由所有者停止、切换配置和重启管理器。重启恢复配置内的许可；普通管理器需重新显式启动代理和确认 UAC，已提升管理器可复用本次令牌。能力和 project-status 显示当前运行许可。

administrator-start 立即返回 administrator_starting 状态，在独立线程等待 UAC；随后查询 administrator-status，直到 administrator_alive:true 或出现 startup_error。等待期间普通项目请求和管理器停止仍可响应。重复 start/status 不另启启动尝试，代理可用后复用同一 PID。administrator-stop 可取消未完成的启动发布，Windows 弹窗仍需点击“否”；若稍后允许，产生的代理也会关闭，不能接收项目。新增项目必须经过配置或 allow 的所有者授权，已有代理通常不另弹 UAC。拒绝启动、跨账户提权、代理死亡均保持管理员后端不可用，不自动重新弹窗或以普通权限执行原管理员请求。dev.12 共享代理仅接受同账户、同会话提权；跨账户路径明确拒绝，旧单实例诊断单独保留。

## 工具身份与审批

六个文件工具及五个任务工具可明确填写 identity:administrator。文件和命令都由同一代理的项目策略检查；没有此参数时，文件使用普通账户，任务使用 restricted/current_user 默认身份。管理员任务的 status/output/input/cancel 也保留 identity，不能通过普通后端或另一项目访问。

```json
{
  "name": "fs_read",
  "arguments": {
    "workspace_id": "work",
    "path": "protected.txt",
    "identity": "administrator"
  }
}
```

管理员能力不会免除 always/workspace_auto 审批。代理内的审批池独立于普通工作进程，使用原有的参数、项目、策略、主体、实例、物理目标版本、期限和预算绑定。always 的文件读取在批准前没有正文；命令及输入分别批准。审批记录不接受 approved=true 等声明。

```powershell
WinShellBot.exe --project-administrator-owner A --request E:\ShellBot\inspect.json --manager-data-dir E:\ShellBot\ManagerData
```

请求文件仍为 {"command":"inspect","id":"审批编号"}；list/approve/deny/revoke 同理。普通 --project-owner 不批准代理中的记录。HTTP project-mcp 与本机 manager-owner 是不同主体，即使项目相同也不能复用对方的审批或任务。

文件访问继续验证实际句柄和配置只读范围，绝对路径仍限显式 current_user 文件范围和本地盘符。管理员读取、搜索、图像、创建、覆盖和修改全部复用原文件工具；哈希冲突、独占编辑、私有备份、分页和重试预算保持有效。管理员程序具有账户/系统能力，不宣称被 cwd 或文件工具目录严格隔离。

命令批准绑定实际工作目录对象的身份和元数据版本、确切程序文件版本、启动时固定的程序内容 SHA-256、参数、超时和请求键。cwd 检查不读取目录内每个文件，因此管理员保护文件不会使同目录下正常的普通命令全部不可用。解释器、脚本或其他依赖不因此自动获得内容固定保证，需按账户信任范围评估；文件列表、搜索和读取的逻辑对象快照保持原有行为。

## 可信通信和生命周期

代理的唯一通信对端是启动它的管理器。私有消息管道拒绝远程访问、固定单连接且不向 worker/task 继承；管理器核对 ShellExecute 或已有提升令牌创建返回的真实进程句柄、管道客户端 PID、实际提升令牌、SID 和会话，代理核对实际服务器 PID、持续持有的管理器进程、同一程序路径和真实令牌。注册的项目 worker 仍独立要求普通身份。SECURITY_IDENTIFICATION 防止普通管理器借用管理员模拟令牌。

项目注册只由该管理器通道发送可信快照；代理再次校验格式、可用身份和普通 worker 的真实进程令牌，持有 worker 句柄并绑定实例。每次操作验证注册项目、配置版本、管理员授权版本、实例、固定主体、规范化请求摘要、单调序号和有界期限，再由代理中的工具策略独立决定是否执行。worker 请求还必须与管理器已认证的原始工具调用完全一致，不能改写参数或选择主体。仅有同账户 SID、项目编号或自报批准不构成管理员授权。

启动时去掉所有项目及独立所有者 credential 环境变量；子任务继续使用固定环境允许列表，仅继承标准输入输出句柄。代理每项目独立任务、幂等、输入、审批、备份和审计池。dev.13 管理员 Data 固定为原项目 Data 下的 administrator-state，管理器仍固定实际目录身份；旧 administrator-实例 目录保留。普通及管理员后端分别计配额，不抵抗已信任账户/管理员程序对其他项目的干扰。

撤销会立即阻止该项目新管理员操作，失效旧审批并取消活动管理员任务；已完成任务的本人状态、输出和取消仍可查询。重新允许产生新授权版本，旧审批不会恢复。project-stop 关闭本项目普通 Job 和代理中的管理员任务及审批，不停止其他项目的代理。worker 异常退出也由代理的独立任务监视器取消所属任务；管理器退出或代理通道关闭时回收全部代理 Job。没有自动任务重放。

撤销和取消不回滚副作用，也不保证已运行管理员代码完全降权。dev.13 增加独立远程所有者入口和受保护历史，重启中断旧批准和任务，不自动重跑。运行期 allow/revoke 仍不写回注册配置；见 [M6](REMOTE_APPROVALS.md)。

## 开发验证

```powershell
# 常规验证不触发 UAC
.\tests\Test-ShellBot-SharedAdministrator.ps1 -Configuration Release
# 首次 UAC 允许，A/B 复用同一代理
.\tests\Test-ShellBot-SharedAdministrator.ps1 -Configuration Release -Administrator
# 独立取消情景：这一次 UAC 点击否
.\tests\Test-ShellBot-SharedAdministrator.ps1 -Configuration Release -CancelStartup
# 两个管理员任务仍运行时停止管理器；这一次 UAC 点击是
.\tests\Test-ShellBot-SharedAdministrator.ps1 -Configuration Release -AdministratorShutdown
```

专项仅使用独立测试配置、目录和原生辅助程序。保护文件由提升的原生程序创建，普通文件工具读取被拒绝；代理完成读取、搜索、图像、创建和带备份修改。原生辅助程序只会终止创建自己的 ShellBot 父进程，用于检验 worker/agent 异常退出，不接受任意 PID。

所有者诊断 --project-administrator-boundary-probe 对已经启动的代理发送固定错误绑定，验证 9 类拒绝，不执行成功的文件访问或修改。该入口不接受任意消息，MCP 不能调用。脚本记录真实 PID/令牌与 UAC 启动尝试；弹窗次数及安全软件观察由用户另行确认。其他 Windows 系统、跨账户流程和真实 MCP 客户端须分别验收。
