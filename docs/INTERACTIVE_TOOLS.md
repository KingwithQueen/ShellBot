# Windows 交互终端、模式搜索与窗口交互（1.1.7）

这些能力在 Windows x64 实现。升级后重新连接 MCP 客户端刷新工具定义；既有项目的工具许可不自动增加。ConPTY 使用原任务工具；UIA 需要显式启用 `uia_tree`、`uia_action` 和相应桌面权限。

## ConPTY 交互终端

普通 current_user 项目的 `task_start` 可选 `io_mode:"terminal"`，创建真正的 Windows 伪控制台，而不是用普通管道模拟 TTY。需要 Windows 10 1809 或更新版本；`host_capabilities.execution_status.terminal_available` 报告当前后端是否可用。受限和管理员执行暂不开放此模式，`launch_mode:"gui"` 与终端模式不能混用。

```json
{"name":"task_start","arguments":{"workspace_id":"work","executable":"C:\\Windows\\System32\\cmd.exe","argv":["/D","/Q"],"request_key":"terminal-1","host_instance_id":"当前实例编号","io_mode":"terminal","terminal_columns":120,"terminal_rows":30,"timeout_ms":300000}}
```

任务审批、调用方归属、幂等键、Job、输出容量、超时和取消规则继续有效。默认 5 分钟超时仍然有效，可在项目上限内指定更长时间。尺寸默认 120×30，列/行分别为 1～1000 个单元格；上限限制伪控制台缓冲的内存占用。启动模式和初始尺寸参与请求身份，重试不能用旧 request_key 切换模式或尺寸。

1.1.7 的任务准备在后台进行，短等后可能返回 `status:"starting"`。此状态不是终态，也不证明进程已经恢复执行；查询原 task_id 的 startup.phase、complete 和 process_resumed，重试启动使用同一 request_key。启动中 task_input 返回含 `task_starting:` 提示的错误，不排队。运行后的输入须先完成必要审计和 input_key 持久意图，再投递给 ConPTY；落盘失败不会提前输入。终态还应等待 output_drained:true、durable_pending:false 后再归档，归档等待错误与安全重试见 [命令任务](COMMAND_TASKS.md#windows-117异步启动与持久化等待)。

本版修复父进程标准流被重定向时错误继承父句柄的问题，确保终端子进程实际绑定 ConPTY 标准句柄；管道模式继续保留独立受控 stdin/stdout/stderr。

使用 `task_input` 发送 UTF-8 文本和终端控制序列。执行命令通常以 `\r` 结束；`\u0003` 表示向这个私有终端发送 Ctrl+C，是否中断取决于应用的控制台模式。它不向物理桌面发送按键。每个新输入使用新的 input_key；同键重试不会重复入队。

```json
{"name":"task_input","arguments":{"task_id":"任务编号","host_instance_id":"当前实例编号","input_key":"command-1","text":"echo 中文\r"}}
```

调整尺寸也使用输入队列：同时提供 `terminal_columns` 和 `terminal_rows`，`text` 可以为空。尺寸与文本在同一条输入记录中按顺序提交；回执 `queued` 不是应用已消费或尺寸已生效的证明。相同 input_key 不能改尺寸。

```json
{"name":"task_input","arguments":{"task_id":"任务编号","host_instance_id":"当前实例编号","input_key":"resize-1","text":"","terminal_columns":160,"terminal_rows":50}}
```

`task_output` 的 stdout 保存 **ConPTY 产生的合并 UTF-8 VT 字节**，stderr 为空。可选择 payload_mode:text/bytes/both；原始 VT 字节和偏移始终保留。VT 包含光标移动、清屏、标题等终端协议，需要终端解析器才能重建屏幕；逐次应用写入可能被控制台合并为画面更新，因此不能把去掉颜色后的文本当作完整编译日志。需要逐字节 stdout/stderr 日志时使用默认 `io_mode:"pipe"`，仍可指定 CP936 等解码。

终端没有管道式 EOF，`eof:true` 会被拒绝，不发送该条输入。发送应用自己的退出命令，或用 `task_cancel` 回收 Job；Ctrl+D/Ctrl+Z 等是否表示输入结束取决于应用。退出、取消或超时后，读取线程继续排空 VT，伪控制台在独立线程关闭，避免关闭过程与输出排空互相等待。`task_status/list/archive` 沿用原契约，不另造一组同义任务接口。

依据：[Microsoft ConPTY 会话](https://learn.microsoft.com/en-us/windows/console/creating-a-pseudoconsole-session)。

## glob 与正则搜索

`fs_search` 新增：

| 参数 | 含义 |
| --- | --- |
| `pattern_type` | literal（默认）、regex 或 glob |
| `ignore_case` | 默认 false；正则和不区分大小写的字面量采用 Unicode 匹配 |
| `file_glob` | 可选文件/名称包含过滤器，作用于相对本次搜索目录的 `/` 路径 |

query 和 file_glob 上限均为 16384 个 UTF-8 字节。glob 的 `*`、`?` 不跨目录，`**` 跨目录，支持字符类；`**/*.c` 也匹配搜索目录中的 `.c` 文件。不是 shell 展开，不执行命令，不支持花括号展开。隐藏文件会搜索；不跟随重解析目录，实际文件仍逐个通过原生目录边界检查。

glob 查询只用于 filename 模式，匹配相对路径；literal/regex 的 filename 模式匹配文件名。content 模式仍按行返回首个匹配及 UTF-8 字节列号，使用 `file_glob` 选择文件。

```json
{"name":"fs_search","arguments":{"workspace_id":"work","path":".","recursive":true,"mode":"content","pattern_type":"regex","query":"^\\s*(TODO|FIXME)\\b","file_glob":"**/*.[ch]","ignore_case":true,"max_results":200}}
```

使用静态 PCRE2 10.49 UTF-8/UCP，不启动 shell 或外部搜索程序，不开启 JIT。无效模式返回错误和 UTF-8 字节位置。已有扫描项数、深度、单文件/总读取量、结果量和约 2 秒搜索预算继续有效；正则另有限制匹配工作量、递归、匹配堆与编译大小。模式耗尽预算时返回已有结果，`truncated:true` 与具体 `truncation_reason`（如 regex_match_budget/regex_time_budget），不伪装成“零匹配”。UTF-8/UTF-16 解码、二进制/不可读文件跳过及截断统计沿用原逻辑。文件 I/O 的等待仍不等于可强制中断的磁盘硬超时。

依赖及授权见 [第三方组件](../third_party/README.md)、[PCRE2 官方项目](https://github.com/PCRE2Project/pcre2)。

## Windows UI Automation

`uia_tree` 读取窗口的 UIA control view。先取得观察或控制租约，再用 window_list 返回的 screen_id/window_id 查询：

```json
{"name":"uia_tree","arguments":{"lease_id":"当前租约","screen_id":"最新观察编号","window_id":"目标窗口编号","request_key":"tree-1","max_nodes":500,"max_depth":16,"include_text":false}}
```

返回不透明 element_id、层级深度、名称、automation_id、类、控件类型、边界、启用/离屏/密码状态和可用 pattern。默认不读取值。显式 include_text:true 才读取非密码控件的 Value/Text，单控件最多 4096 个 UTF-16 单元，保守标记 text_may_be_truncated。提供者声明为密码或密码状态不可确认时，不请求其文本，名称留空。应用自己未正确声明的控件只能按其 UIA 数据处理。

节点及界面文本只保留在当前实例内存和本次回执中，不写入桌面操作的持久历史；历史保留操作元数据。树读取按原读取回执期限过期，不跨进程恢复元素身份。

默认最多 500 节点、深度 16，可分别提高到 1000/32；还有总输出和时间预算。树可能截断，不能根据缺失节点断言控件不存在。仅接入同一普通账户、会话、权限级别内的可见顶层窗口及其同进程元素；跨进程的嵌入控件可能缺失。没有可访问性提供者的画布/游戏界面可能只有很少节点。

`uia_action` 支持 invoke、set_value、toggle、select、expand、collapse。默认 `focus_mode:"foreground"`，先用控制租约将目标窗口聚焦，再刷新 window_list、uia_tree，使用同一观察中的元素：

```json
{"name":"uia_action","arguments":{"lease_id":"当前控制租约","screen_id":"对应观察编号","window_id":"对应窗口编号","element_id":"最近树中的控件编号","request_key":"set-value-1","action":"set_value","value":"新的文本"}}
```

value 只用于 set_value，最高 16384 个 UTF-8 字节。动作会再次核实窗口/进程创建身份、元素运行身份及名称/类型/标识、人工暂停、租约和权限。拒绝密码、禁用、离屏、只读或状态未知的相应控件，不用键鼠或坐标回退。观察租约不能执行动作。树缓存仅保留最近一次并绑定当前窗口和观察；任何动作尝试后都要重新观察。

1.1.4 新增显式 `focus_mode:"background"`。它直接调用观察到的目标控件 pattern，不要求窗口已经前台，也不请求聚焦或发送键鼠。默认 interactive 租约要求未锁定、同用户的可用输入桌面，包括 mstsc 最小化后像素不可获取但 UIA 提供者仍可用的情况；同用户会话锁定/断开时，应另行申请下文的 application_background 租约：

```json
{"name":"uia_action","arguments":{"lease_id":"当前控制租约","screen_id":"对应观察编号","window_id":"对应窗口编号","element_id":"最近树中的编辑框编号","request_key":"background-value-1","action":"set_value","value":"中文内容","focus_mode":"background"}}
```

后台模式仍需控制许可/租约、所有者未暂停及稳定的窗口/进程/控件身份，且目标应用窗口不能最小化，控件必须启用且在屏幕范围内。默认 interactive 模式还要求未锁定的 Default 输入桌面；显式 application_background 模式可访问同用户锁定/断开会话中的普通 Default 应用。它不解除人工暂停，不操作登录/UAC 界面，不放宽跨用户/会话/权限边界；应用自身的提供者可能仍要求前台，或在调用时主动显示自己的界面。无相应 pattern 的画布/游戏不能据此后台交互。按键、组合键、坐标点击、拖动和真实前台聚焦仍使用原来的桌面工具。

`uia_tree` 和显式后台动作以目标窗口/元素为准，不因无关窗口的前台或矩形变化判定过期；人工输入 epoch、观察期限、显示布局和目标窗口身份变化仍会阻止调用。默认前台动作保留原来的前台观察检查。回执报告 focus_mode、foreground_required、foreground_observed，后台派发成功不等于已聚焦。

UIA 辅助线程采用 Per Monitor V2 DPI 感知，树的 `coordinate_space:"physical_desktop_pixels"` 与 window_list 一致，避免 RDP 150% 等缩放下混用逻辑像素。

UIA 在一次性独立 MTA 进程中运行，提供者连接/事务等待各设为 500 ms，整次请求最多约 4 秒；无响应可回收辅助进程。动作超时或失败可能已有部分效果，回执标记 unknown，禁止自动重放。dispatched 只说明 pattern API 返回成功，`application_result_verified:false` 明确要求调用方刷新核实。UIA 不证明截图可用，不自动取得 Windows 前台资格，不操作登录/UAC 界面，也不绕过人工暂停。

UIA 请求进行中不会并行执行其他桌面工具，状态查询和租约释放仍可处理；人工暂停继续立即生效。不要把并发请求返回的 busy 当作已经执行。

依据：[UIA 线程模型](https://learn.microsoft.com/en-us/windows/win32/winauto/uiauto-threading)、[IUIAutomation2](https://learn.microsoft.com/en-us/windows/win32/api/uiautomationclient/nn-uiautomationclient-iuiautomation2)。本机真实 mstsc 最小化下的自建 Win32 窗口已验证后台中文写入/读回和按钮单次调用；其他实际应用、提供者和系统仍需分别验收，见 [RDP 记录](REMOTE_DESKTOP.md#114-实机对照与后台语义操作)。

## 不聚焦的窗口截图（1.1.5）

复用 `desktop_capture`，指定 window_id 和 screen_id 后进入窗口模式。先取得 observe 或 control 租约，通过 window_list 选择目标；窗口截图不要求前台，不激活或恢复窗口，也不会回退为屏幕 DC 截图：

```json
{"name":"desktop_capture","arguments":{"lease_id":"当前租约","screen_id":"最近 window_list 的观察编号","window_id":"该观察中的目标窗口编号","request_key":"window-image-1","capture_backend":"auto","max_dimension":1600,"response_mode":"compact"}}
```

- capture_backend 默认 auto，先使用 WGC；无帧、失败或疑似全黑时尝试 PrintWindow。可明确指定 wgc 或 printwindow 诊断，指定后只运行该后端。它仅用于窗口模式。
- 同一请求不能混用窗口模式和 monitor_id、x/y/width/height；省略 window_id 时原来的桌面/区域截图契约保持不变。窗口模式必须提供 screen_id。
- 继续要求同用户、同会话、权限范围内的可见非最小化窗口和稳定进程创建身份。默认 interactive 租约拒绝锁屏/断开，显式 application_background 租约可捕获该用户 Default 桌面的普通应用；登录/UAC 界面不在范围内。身份/布局/epoch 变化、过期观察或租约仍会拒绝。观察租约允许在所有者暂停时截图，但不能输入。
- max_dimension 与现有 PNG 容量上限保持不变。像素捕获/编码使用独立普通用户进程，应用绘图或驱动卡住可单独回收；每次窗口捕获总预算约四秒，受剩余租约和原请求期限约束。大 PNG 用独立线程持续排空私有管道，同时检查取消和目标，避免逐块等待限速。

回执的 capture_diagnostic 包含各后端的阶段、HRESULT、Win32 错误、像素/编码状态、输出字节数和图像验证结果，以及最终 selected_backend。PrintWindow 依赖应用配合，API 成功并不证明所有控件或 GPU 区域都正确绘制；全黑图仍可作为有效 PNG 返回，但 content_may_be_blank:true 会明确提示。application_paint_verified:false 要求调用方结合文字状态和后续图像确认效果，不能把时间戳不同等同于应用画面更新。

1.1.7 另返回 frame_freshness，status 固定为 unverified，application_update_verified:false。PrintWindow 的 cached_surface_possible:true 提示可能只取得旧 GPU 合成面；WGC 的 compositor_frame_age_ms 在可计算时表示合成器帧龄，否则为 null。WGC 的 cached_surface_possible:false 不代表应用已经处理本次操作：合成器时间戳不是应用完成确认。应结合 UIA 读回、应用状态与动作后的变化像素核实，不把截图 API 成功当作当前画面证明。

图片使用 `coordinate_space:"window_image_pixels"`，`coordinate_input_source:false`。WGC 通常不含不可见窗口边框，PrintWindow 使用完整窗口；有可靠映射时返回 physical_region 与 physical_pixels_per_image_x/y，否则 physical_mapping_known:false、physical_region:null。PrintWindow 匹配目标应用的 DPI 上下文，避免 150% 等缩放时只在左上角绘制、其余留黑。

窗口截图本身不赋予全局坐标输入许可。后台操作使用 UIA：截图后刷新 window_list/uia_tree，再用 `uia_action focus_mode:background` 选择观察到的控件。执行后重新截图并核对。真实按键、坐标点击/拖动和 window_focus 仍沿用原来的前台与输入保护条件。

同一项目有窗口捕获进行时，其他桌面操作返回 desktop_busy_capture；状态/能力查询及租约释放仍可处理，所有者暂停不受阻塞。窗口像素不保存到持久历史，compact 回执保留一份图片而不重复完整 JSON。

本机真实 mstsc 最小化期间，普通 Win32 窗口的 WGC、PrintWindow、auto，以及 D3D11 画布的 WGC、auto，均完成后台中文写入/读回、单次按钮调用、橙/蓝/橙两次真实画面变化的验证。两类窗口均未取得前台。详细范围见 [RDP 实机结果](REMOTE_DESKTOP.md#115-最小化时的窗口截图和后台操作)。

## 锁屏时的应用后台模式（1.1.6）

常规租约仍默认 `session_mode:"interactive"`，保留锁屏/断开/UAC 拒绝规则。需要访问已登录用户在 Default 桌面上的普通应用时，显式申请：

```json
{"name":"desktop_lease","arguments":{"action":"acquire","access":"control","session_mode":"application_background","duration_seconds":30,"request_key":"background-app-1","response_mode":"compact"}}
```

仅查看可用 `access:"observe"`，它允许在人工暂停时读取窗口、UIA 树及窗口截图，始终禁止控制。control 仍受项目控制许可和所有者暂停约束。用 host_capabilities.desktop_status.application_background_available 判断应用后台上下文，不能用常规 desktop_control:false 推断它也不可用。

后台租约只允许 window_list、desktop_state、desktop_capture 的窗口模式、uia_tree，以及显式 `focus_mode:"background"` 的 uia_action。普通桌面整体抓屏、聚焦、真实按键/鼠标、剪贴板、前台 UIA 操作均被拒绝。续期保留模式，不能把观察租约或普通租约升级为后台控制。

保持原用户/会话/权限、窗口进程创建身份、观察期限、布局、人工暂停及输入 epoch 检查；锁定或连接状态发生变化时旧后台租约失效，应重新观察并申请。工具不解锁、不登录、不切换输入桌面、不接触登录/UAC 界面；已连接且未锁定、但输入桌面不可访问的 UAC 状态仍被拒绝。

流程为：申请后台租约 → window_list → 窗口截图/读取 UIA → 后台语义操作 → 刷新并核对结果 → 释放租约。每个新请求使用新 request_key。截图优先 WGC；断开且锁屏时 WGC 可能无帧，auto 会兼容回退 PrintWindow。PrintWindow 的 GPU 内容可能是旧画面，即使返回 PNG 成功，也不能当作操作结果证明。[远程桌面说明](REMOTE_DESKTOP.md#116-断开并锁屏时的应用后台模式)保留 1.1.6 的旧帧失败；1.1.7 可按下节显式选择受支持的 GPU 配套路径。

1.1.6 自动回收已结束的操作回执缓存，数十次连续观察不再占死 64 个槽；已回收输入/写入的旧请求键返回 operation_receipt_reclaimed，绝不重新执行。容量含义见 [回执回收](CAPACITY_LIMITS.md#116-操作回执回收)。

## 显式 GPU 配套取图（1.1.7）

锁屏时优先使用无需虚拟机的两条适配路径：所有者已配置本机调试端口的独立 Chromium 页面使用只读截图脚本；新启动的普通权限 x64 D3D11 应用可显式使用 ShellBotGraphics。后者只覆盖经典 D3D11CreateDeviceAndSwapChain 加 Present 的首个受支持交换链、非 MSAA、非 HDR 的 BGRA/RGBA8 后缓冲，不覆盖 Present1、Factory 创建交换链、D3D12、OpenGL、Vulkan 或跨进程 GPU 渲染。Chromium/Electron 应用也须自己提供兼容调试协议，不能据此宣称所有游戏或 Electron 均可用。

配套程序通过明确的命令执行许可启动，不由观察租约自动启用；普通 desktop_capture 不附着或注入现有应用，桌面工具的默认保护保持有效。GPU 图片是应用表面，不赋予全局坐标、焦点或物理键鼠权限。用法、新鲜度参数及停止流程见 [GPU 取图说明](GRAPHICS_CAPTURE.md)，Z620 的实际 UIA 中文读回、两次按钮及 GPU 新像素核对见 [1.1.7 实测](REMOTE_DESKTOP.md#117-锁屏时的-gpu-新画面)。
