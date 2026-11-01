# Windows 交互终端、模式搜索与 UIA（1.1.3）

这些能力在 Windows x64 实现。升级后重新连接 MCP 客户端刷新工具定义；既有项目的工具许可不自动增加。ConPTY 使用原任务工具；UIA 需要显式启用 `uia_tree`、`uia_action` 和相应桌面权限。

## ConPTY 交互终端

普通 current_user 项目的 `task_start` 可选 `io_mode:"terminal"`，创建真正的 Windows 伪控制台，而不是用普通管道模拟 TTY。需要 Windows 10 1809 或更新版本；`host_capabilities.execution_status.terminal_available` 报告当前后端是否可用。受限和管理员执行暂不开放此模式，`launch_mode:"gui"` 与终端模式不能混用。

```json
{"name":"task_start","arguments":{"workspace_id":"work","executable":"C:\\Windows\\System32\\cmd.exe","argv":["/D","/Q"],"request_key":"terminal-1","host_instance_id":"当前实例编号","io_mode":"terminal","terminal_columns":120,"terminal_rows":30,"timeout_ms":300000}}
```

任务审批、调用方归属、幂等键、Job、输出容量、超时和取消规则继续有效。默认 5 分钟超时仍然有效，可在项目上限内指定更长时间。尺寸默认 120×30，列/行分别为 1～1000 个单元格；上限限制伪控制台缓冲的内存占用。启动模式和初始尺寸参与请求身份，重试不能用旧 request_key 切换模式或尺寸。

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

`uia_action` 支持 invoke、set_value、toggle、select、expand、collapse。先用控制租约将目标窗口聚焦，再刷新 window_list、uia_tree，使用同一观察中的元素：

```json
{"name":"uia_action","arguments":{"lease_id":"当前控制租约","screen_id":"对应观察编号","window_id":"对应窗口编号","element_id":"最近树中的控件编号","request_key":"set-value-1","action":"set_value","value":"新的文本"}}
```

value 只用于 set_value，最高 16384 个 UTF-8 字节。动作要求目标窗口已经是前台；会再次核实窗口/进程创建身份、元素运行身份及名称/类型/标识、人工暂停、租约和权限。拒绝密码、禁用、离屏、只读或状态未知的相应控件，不用键鼠或坐标偷偷回退。观察租约不能执行动作。树缓存仅保留最近一次并绑定当前窗口和观察；任何动作尝试后都要重新观察。

UIA 在一次性独立 MTA 进程中运行，提供者连接/事务等待各设为 500 ms，整次请求最多约 4 秒；无响应可回收辅助进程。动作超时或失败可能已有部分效果，回执标记 unknown，禁止自动重放。dispatched 只说明 pattern API 返回成功，`application_result_verified:false` 明确要求调用方刷新核实。UIA 不证明截图可用，不自动取得 Windows 前台资格，不绕过 UAC、锁屏或人工暂停。

UIA 请求进行中不会并行执行其他桌面工具，状态查询和租约释放仍可处理；人工暂停继续立即生效。不要把并发请求返回的 busy 当作已经执行。

依据：[UIA 线程模型](https://learn.microsoft.com/en-us/windows/win32/winauto/uiauto-threading)、[IUIAutomation2](https://learn.microsoft.com/en-us/windows/win32/api/uiautomationclient/nn-uiautomationclient-iuiautomation2)。真实 RDP 最小化及不同实际应用的界面行为仍需在相应环境验收。
