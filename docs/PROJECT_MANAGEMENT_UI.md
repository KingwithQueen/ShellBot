# M7 本机项目管理窗口

版本 `0.1.0-dev.14`，Windows 原生窗口，无浏览器或额外运行时依赖。窗口通过受保护的本机所有者通道连接已经启动的管理器。

## 打开

先用已有注册配置启动普通管理器，再打开窗口；两处 manager-data-dir 必须指向同一位置。项目凭据仍由管理器启动时的环境提供，窗口不读取或保存凭据。

```powershell
$bot = 'E:/Program/ShellBot/out/x64/Release/WinShellBot.exe'
& $bot --manager --http --projects E:/ShellBotSettings/projects.json --manager-data-dir E:/ShellBotProjects/manager/state
# 在另一个普通终端打开窗口
& $bot --manage --manager-data-dir E:/ShellBotProjects/manager/state
```

窗口接受普通或已提升的交互本机所有者身份；项目工作进程仍保持普通权限。未连接时显示通道错误；启动管理器后点击刷新。关闭窗口保留管理器和任务。只有显式停止项目、暂停、停止代理或管理器才结束相应任务。

## 操作

项目表显示当前状态、审批方式和默认任务身份；选择项目后显示文件范围、只读目录、管理员许可、代理状态和 MCP 地址。地址不含令牌，公网地址未填写时复制按钮不可用。网关未运行时地址仍可查看，界面另显示网关未启动；地址存在不代表外部网络已连接。

启动和恢复只作用于所选项目。停止/暂停前明确确认会结束活动任务及失效许可，其他项目继续运行。暂停设置本管理器运行期 paused，并撤离该项目普通/管理员任务后端；HTTP 返回不可用，project-start 拒绝，必须明确 project-resume。恢复创建新实例，从历史显示中断/实际终态，不重跑任务、不重新投递 stdin，也不恢复旧批准。暂停不写回配置；管理器重启后注册项目均停止。

启动管理员按钮先确认，再复用原异步启动协议；普通管理器请求 UAC，已提升管理器复用已有令牌。项目许可与代理是否提升分别显示。停止代理先确认会影响全部项目管理员任务。窗口不因刷新、项目启动或恢复自动提升，不修改项目管理员许可。dev.19 的已提升托盘还可登记当前账户的最高权限登录计划任务，见 [启动配置](STARTUP.md)。

查看诊断对运行项目查询实际 host_capabilities；停止项目显示配置和最后状态。诊断中的 restricted_backend_verified 等实际证据与默认身份配置分别表述。查询可能返回忙或失败，界面保留错误，不自动重发有副作用操作。复制诊断由用户明确操作。请求在独立线程执行，关闭窗口不重试请求；已送达的操作仍可能完成，应重新连接核对状态。

刷新读取当前快照，不自动执行任务。配置创建/更新仍通过现有命令导出新注册文件，保存原配置并明确重启；窗口当前不支持直接编辑、删除、在线重载或自动启动管理器。

窗口按 DPI 布局并支持调整大小，当前最小逻辑窗口约 1020×660；较小有效显示区域须使用命令行管理。多屏 DPI 切换与其他 Windows 系统仍需独立验收。

## 命令与边界

```powershell
& $bot --management-status --manager-data-dir E:/ShellBotProjects/manager/state
& $bot --project-pause example --manager-data-dir E:/ShellBotProjects/manager/state
& $bot --project-resume example --manager-data-dir E:/ShellBotProjects/manager/state
```

management-status 在本机通道提供配置路径、目录权限、令牌变量引用、地址及实际运行状态，不返回凭据值。远程 /owner/api 与项目 MCP 不能调用这些管理操作；原有限远程审批权限保持。普通账户/管理员程序仍保留系统权限，本机窗口不构成隔离沙箱。

## 候选包

新 scripts/package-candidate.ps1 只接受 M7 当前源码与程序对应的本机回归、实际窗口及提权报告，生成新的独立 candidate 目录和 ZIP。它不修改旧 scripts/package-test.ps1 的 dev.7 证据门槛。候选包包含主程序、产品 SandboxProbe、只读示例、管理说明、许可证、当前报告和摘要；不包含开发夹具、用户 Data 或凭据。

解压后复制 manager.example.json 为 projects.local.json，填写已存在的工作目录及独立项目 Data 位置，再在普通终端设置 SHELLBOT_EXAMPLE_TOKEN 与独立 SHELLBOT_OWNER_TOKEN。省略 owner_token_env 可关闭远程审批。示例默认每次审批、只读、关闭任务和管理员；不会自动创建工作目录或扩大权限。

```powershell
$env:SHELLBOT_EXAMPLE_TOKEN = (& .\WinShellBot.exe --generate-token)
$env:SHELLBOT_OWNER_TOKEN = (& .\WinShellBot.exe --generate-token)
.\Start-Manager.cmd
# 管理器运行后另开窗口，或者双击 Open-Management.cmd
.\Open-Management.cmd
```

启动脚本使用解压目录 Data/Manager；GUI 与管理器一致。不存在 projects.local.json 时脚本显示配置说明并退出。示例与实际配置分开，候选包本身不带 Data；运行生成的数据应单独保留，升级不会自动迁移、替换或删除。

本机 Windows 11 自动/UI/UAC 结果与独立 Windows 10、Server 2025、另一设备、已有代理和真实 MCP 客户端分别记录。历史 HTTP close 重置仍待调查，候选包声明 core_except_connection_close，不能当作正式完成所有平台验收。
