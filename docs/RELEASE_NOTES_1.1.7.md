# ShellBot 1.1.7 发布说明

本版继续优先完善 Windows x64，正式包命名为 ShellBot-1.1.7-windows-x64.zip。Mac/Linux 尚未重新移植。本轮主要处理慢任务启动、任务持久化与 ConPTY，并提供锁屏时无需虚拟机的显式 GPU 取图路径。

## 任务启动和归档

task_start 将持久预留、环境准备、原生创建和启动记录放到后台。后端短等最多约 250 ms，未就绪时返回 status:starting 和同一个 task_id；这是非终态，不是执行失败或进程已运行的证明，也不是整次网络请求的总时限。startup 提供当前阶段、分阶段耗时、是否恢复进程及真实失败位置/原生错误。相同 request_key 始终查询原任务，不因等待或未知回执重放命令。

进程在创建时原子加入任务 Job 并保持挂起，I/O 和必要启动意图准备好后才恢复。慢状态日志、审计、输入意图、完成及归档写入不在全局任务锁中等待；状态查询使用已提交快照。task_input 在 starting 时明确拒绝，运行后的输入也必须先完成必要审计与持久意图，才能向管道或 ConPTY 投递。

任务完成、输出排空与持久化收尾分别报告。TASK_PERSISTENCE_PENDING 和 TASK_ARCHIVE_PENDING 是可按同一 task_id 重试的明确状态；它们不释放输出或记录额度，只有 archived:true 才证明归档完成。批量 pending_task_ids 继续只表示调度预算内未尝试的项。保留已结束操作回执自动回收和防重放摘要。详细字段见 [命令任务](COMMAND_TASKS.md)。

ConPTY 修复父进程标准流被重定向时错误继承父句柄的问题，确保终端子进程实际绑定伪控制台；普通管道仍使用独立 stdin/stdout/stderr。退出继续先排空输出，再完成终端关闭。终端模式、输入及 EOF 规则见 [交互工具](INTERACTIVE_TOOLS.md)。

## 锁屏应用和 GPU 取图

普通 current_user GUI 任务可以在自身同用户会话锁定/断开时，向 WinSta0/Default 创建应用，不要求输入桌面或截图先成功。后续普通应用观察和后台 UIA 需要显式 application_background 租约；真实焦点和键鼠仍要求可用的交互输入桌面。不登录、不解锁、不切换用户会话，也不操作登录/UAC 界面。

WGC/PrintWindow 窗口截图新增 frame_freshness，明确 status:unverified、application_update_verified:false。PrintWindow 可能返回旧 GPU 合成面；WGC 的合成器帧龄也不等于应用已完成操作。原 1.1.6 在 Z620 锁屏时返回旧色块的失败保留为历史证据，不能用 PNG 成功或时间戳变化代替当前画面验证。

本版提供两条显式、无需虚拟机的适配路径，见 [GPU 取图说明](GRAPHICS_CAPTURE.md)：

- **Chromium 页面**：只读配套脚本连接所有者已配置的本机调试端点，验证端点所有者和指定页面后取图；不扫描个人资料，不执行任意脚本、输入或焦点仿真，不自动打开调试端口。
- **新 D3D11 应用**：ShellBotGraphics 以明确 opt-in 参数托管新启动的同用户、普通权限 x64 程序，采集提交前的后缓冲纹理；同一 Job 管理退出、超时和停止，帧携带身份、序号和时间，读取可要求动作后的帧及最大帧龄。不附着已有用户进程，不热卸载。

D3D11 当前只覆盖经典 D3D11CreateDeviceAndSwapChain 加 Present 的首个受支持交换链、非 MSAA、非 HDR 的 BGRA/RGBA8 后缓冲。不覆盖 Present1、Factory 创建交换链、D3D12、OpenGL、Vulkan 或跨进程 GPU 渲染；Chromium/Electron 也须提供兼容调试协议。不能据此宣称任意游戏、Electron 或全部 GPU API 均已支持。普通桌面工具不会自动注入应用，配套程序仍须明确命令执行许可。

## 已完成的实机验证与范围

Z620 保持断开并锁屏，正式冻结的 ShellBotGraphics 程序完成自建 D3D11 窗口的后台 UIA 中文写入/读回和两次按钮调用；橙→蓝→橙三张图均核实当前色块。最终 Chromium 脚本在独立 Edge、Quadro K6000、D3D11/WebGL 页面上完成三张变化截图，每张 83520 个当前色块像素、旧色块 0，没有启用焦点仿真。详见 [远程桌面实测记录](REMOTE_DESKTOP.md#117-锁屏时的-gpu-新画面)。

最终核心在 Z620 锁定/断开状态另外完成 1254 项检查：100 个私有 PowerShell 脚本任务逐项验证输出并归档，最终活动/实时记录均为 0、归档数 100；初次回执 87 个 running、13 个 starting，最大观测启动耗时 297 ms。同一 request_key 重试的标记仅一行。真实 ConPTY 输出及结束、GUI 模式启动、后台 UIA 值写入/读回和普通 PrintWindow 新鲜度诊断均通过，最终会话仍为 locked/disconnected。

这些是所列程序、GPU 路径和真实锁屏环境的专项结果，不代替任意应用、物理键鼠或长期运行验收。ShellBotGraphics 本机 Release/Debug 已完成生命周期、真实 D3D11 尺寸/格式/调整缓冲和安全边界验证；整轮回归及原生测试以最终验证摘要和原始收据为准，不由专项结果推定成功。原始失败和历史报告保留，不覆盖旧版证据。本轮未创建虚拟机/账号、保存登录凭据或更改显示驱动。

## 升级

从 1.1.6 或更早版本升级时，旧更新器的固定运行文件清单会拒绝新增的 ShellBotGraphics.exe / ShellBotGraphicsCapture.dll。先正常停止 ShellBotGraphics 托管的应用并退出 ShellBot，备份数据、位置设置和旧更新器；从可信的 1.1.7 ZIP 单独取出新版 ShellBotUpdate.exe 替换安装根目录的旧更新器，再用它安装完整 ZIP。不要覆盖或删除 Data、ShellBot.storage.json，保留原 Windows 账户。完整步骤、失败恢复和手动升级见 [更新说明](UPDATING.md)。

升级后重新启动 ShellBot，并重新连接 MCP 客户端、刷新工具定义；项目权限不会自动增加。版本之后按末位进位规则递增：第三位、第二位均为 0～9，9 后进位；第一位为不设上限的正整数。
