# Windows 桌面工具（1.0.7）

## 显式按键与组合键

Windows 的 `desktop_key` 现支持 `key` 和可选的 `modifiers`。它发送真实虚拟键的按下／释放事件，方便分别验证 Enter、Shift+Enter 和 Ctrl+V。例如以下是三个独立调用的按键参数，调用时都需要补上本人 `lease_id`、刚取得的 `screen_id` 和新的 `request_key`：

```json
{"key":"ENTER"}
{"key":"ENTER","modifiers":["SHIFT"]}
{"key":"V","modifiers":["CTRL"]}
```

完整调用示例（将占位标识替换为实际值）：

```json
{"name":"desktop_key","arguments":{"lease_id":"当前租约标识","screen_id":"最新观察标识","request_key":"shift-enter-1","key":"ENTER","modifiers":["SHIFT"]}}
```

按键名不区分大小写；支持 ENTER/RETURN、TAB、ESC/ESCAPE、BACKSPACE、DELETE、HOME、END、PAGEUP、PAGEDOWN、UP/DOWN/LEFT/RIGHT、SPACE、A–Z、0–9、F1–F12。组合修饰键为 CTRL/CONTROL、ALT、SHIFT，最多三个且不能重复。按修饰键数组的顺序按下，再按主键，最后按相反顺序释放全部键。旧 `keys:["SHIFT","ENTER"]`、`keys:["CTRL","V"]` 继续支持；两种参数格式不能混用，不能单独发送或跨调用保持修饰键按下。安全桌面组合键及配置的紧急停止组合仍拒绝注入。

`desktop_type` 输入 Unicode 文本，文本中的换行不能替代 Enter 虚拟键。`clipboard_write` 只写入剪贴板：如需粘贴，先核对剪贴板序号并写入，再取得新观察，用 `desktop_key` 提交 Ctrl+V。实际提交、换行或粘贴效果由目标应用决定，ShellBot 不会把键盘事件接收当作业务成功。

回执增加规范化的 `key`、`modifiers`、完整 `keys` 数组和 `events_requested / events_inserted`。Enter 请求两个事件，Shift+Enter、Ctrl+V 各四个。`key_submission_status` 为 `complete`、`partial` 或 `not_inserted`；部分提交会尝试释放本次已提交且尚未释放的键，`cleanup_events_requested / cleanup_events_inserted` 报告释放结果。`win32_error`、`win32_error_available` 保留原始提交错误；有些 Windows 输入拒绝不提供扩展错误，此时错误码 0 不能当成成功。

输入通过单批 [Windows SendInput](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendinput) 按顺序提交；清理根据返回的已提交事件数量进行配对，不代表应用已经处理了所有事件。

`application_success_confirmed` 保持 false。每次新动作后重新观察并核对应用结果；失败或超时先检查回执，不自动重复按键或粘贴。相同请求键和参数的重试仍重放原回执，避免重复输入。权限、审批、短时租约、人工暂停和窗口过期检查继续有效。`host_capabilities.desktop_status` 通过 `explicit_key_chords`、`max_key_modifiers`、`key_hold_supported` 公布能力。Mac/Linux 暂沿用原 `keys` 格式。

## 观察过期与截图诊断

`stale_observation` 回执增加 `code`、`changes` 和恢复建议。`changes` 每项标明 `field`、`expected` 和 `actual`，窗口变化附观察中的 `window_id`、`pid`；前台切换同时报告窗口句柄和进程编号，位置变化报告前后边界。其他原因包括 `monitor_layout`、`input_epoch`、`observation_age_ms`、动作已使观察失效或 `screen_id` 已被最近八份观察淘汰。窗口聚焦还报告 `requested_window_id`；目标窗口本身失效时返回 `stale_window_id` 及其位置、进程、线程、可见性或最小化状态变化。窗口句柄仅用于诊断，操作继续使用不透明 window_id。

刷新后仍可能因窗口正在移动、前台切换或人工输入而立即过期；根据变化先稳定目标，再重新观察并使用新的请求键。1.0.6 修复暂停/恢复定时器使刚刷新观察再次失效的竞态，旧观察的输入代次检查继续有效。

`desktop_capture` 的结果增加 `capture_diagnostic`。错误分为 `capture_failed`、`png_encoding_failed`、`png_budget_exceeded`、`image_serialization_failed`，分别表示 GDI 抓屏、PNG 编码、PNG 体积和结果序列化失败。只有体积超限需要优先尝试减小区域或尺寸，不再将各种失败合并为同一消息。

- `stage` 指明 GetDC、StretchBlt 或具体 WIC 阶段；`win32_error` 及 `win32_error_available` 保留原生错误，`hresult` 为编码阶段 HRESULT 的无符号数值，未进入编码时为 null。某些 GDI 失败不提供扩展错误，此时不把 0 解释为成功。
- `encoded_bytes` 是已知的实际 PNG 字节数，`png_limit_bytes` 为上限（16 MiB）。编码未完成时为 null，`encoded_bytes_known:false`；不会把未知长度报告为 0。
- `desktop_session_available` 表示当前交互桌面是否符合条件；`pixels_captured` 表示本次实际抓屏是否成功。`desktop_capturable` 在实际抓屏成功时为 true，抓屏失败或会话不可用时为 false，桌面符合条件但尚未尝试时为 null。PNG 编码失败不等于桌面不可捕获。

以上诊断不保证受保护窗口内容可见，也不解除锁屏、断开会话或安全桌面的限制。

dev.15 为普通、新格式项目提供当前用户会话的截图、窗口列表和鼠标键盘工具，以及独立的纯文本剪贴板权限。旧单实例入口和管理员代理不提供这些工具。产品使用 Windows 原生接口，无需 Python、Node.js 或额外桌面运行库。

桌面权限默认关闭，文件目录授权不会自动开放桌面。桌面观察可能看到整个会话的窗口标题和屏幕内容；显式开启后，目录只读或受限命令身份不会约束桌面内容。不要把这一权限当作目录隔离。输入拒绝更高完整性、未知归属或其他账户的窗口，不操作 UAC、锁屏和登录界面。

## 配置

在项目 `policy` 中加入：

```json
"desktop": {
  "enabled": true,
  "control": false,
  "clipboard_read": false,
  "clipboard_write": false,
  "lease_seconds": 30,
  "emergency_key": "F9"
}
```

这是片段，完整只读示例见 [desktop.example.json](../config/desktop.example.json)。示例允许审批后的桌面观察，文件、命令、输入和剪贴板关闭。根据需要将 `control` 或对应剪贴板权限改为 `true`，并在 `policy.tools` 中添加对应工具；仅修改 `desktop.enabled` 不会扩大已有工具列表。按既有配置导出、校验和重启流程应用，运行中的配置不会偷偷更改。

`lease_seconds` 为项目最长单次期限，1–120 秒，默认 30。`emergency_key` 支持 F1–F11，固定搭配 Ctrl+Alt，默认 Ctrl+Alt+F9。若热键已被其他软件占用、输入监测钩子无法安装或会话不可用，取得控制失败；更换配置后明确重启项目。F12 是 Windows 保留的调试热键，配置不接受它。[微软 RegisterHotKey 说明](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-registerhotkey)

这里的秒数是一次桌面控制租约的有效期，涵盖桌面观察、截图、窗口、键鼠和剪贴板工具；不是项目运行时长、后台命令超时或 OAuth 授权期限。客户端可在到期前调用 `desktop_lease` 的 `renew`，新的期限从续租成功时起算，仍受项目上限与审批规则约束；一般桌面调用不会自动续租。到期后旧租约失效，可重新申请；项目、文件工具和命令任务继续运行。已经提交给 Windows 的输入不会因租约到期而撤销。

`never` 在上述明确授权范围内自动执行；`always` 和 `workspace_auto` 对每次新的观察、输入和租约取得/续期都要求所有者审批。审批不会扩大工具和输入权限。所有者本地点击审批页面也属于人工输入，会暂停桌面；逐次审批的连续操作适合由另一设备的所有者页面批准。恢复后必须重新观察，不能继续用恢复前的截图。AI 不能自行批准或解除暂停。

## 调用顺序

1. 查看 `host_capabilities.desktop_status`，确认实际会话可用和权限；工具发现只列出当前可用且获准的桌面工具。
2. `desktop_lease` 的 `action: "acquire"` 取得短期独占控制，保留 `lease_id`。遇到 pending，按已有所有者审批流程批准，然后带 `approval_id` 重试相同参数。
3. 用 `desktop_state`、`window_list` 或 `desktop_capture` 取得 `screen_id`、窗口标识和坐标。观察最长有效 30 秒，最多保留最近 8 份。
4. 输入时带 `lease_id`、`screen_id` 和新的 `request_key`。坐标使用返回的物理桌面像素，截图若缩小，按 `physical_pixels_per_image_x/y` 与 `physical_region` 换算。
5. 每次动作后重新观察，检查应用实际结果。前台、窗口位置、显示器布局、人工输入或暂停/恢复发生变化时，旧观察不能驱动输入。
6. 结束时 `desktop_lease` 使用 `action: "release"`；`status` 和释放本人控制无需逐次审批。续期使用 `action: "renew"`、原 `lease_id` 和新的请求键。

同一账户的同一 Windows 会话跨项目、跨管理器只允许一份控制。租约绑定项目、普通工作进程实例及认证调用身份；同一项目 Bearer 的多个客户端共用该认证身份，本版不建立独立 MCP 会话。新工作进程不能使用旧租约。过期、工作进程退出或会话失效会释放控制，不自动取得新租约或重跑输入。人工暂停会保留本人租约至到期，避免其他项目抢占。

每次新的请求使用不同的 1–64 字符 `request_key`（字母、数字、下划线、连字符和点）。完全相同的重试返回内存回执，不再次点击、输入、续期或写剪贴板；修改同一请求键的参数会被拒绝。缓存图片只是原请求回执，其 `screen_id` 仍受观察时限约束。请求超时后先检查实际效果；新请求键表示新动作，不是无副作用重试。

## 工具与限制

| 工具 | 参数和实际行为 |
| --- | --- |
| `desktop_lease` | `action`；acquire/renew 可选 `duration_seconds`，不能超过项目上限；renew/release 需要本人 `lease_id` |
| `desktop_state` / `window_list` | `lease_id`、`request_key`；当前会话、显示器物理范围/DPI、剪贴板序号、最多 128 个可见窗口与不透明 `window_id` |
| `desktop_capture` | 可选 `monitor_id`（稳定排序、从 0 开始）、`max_dimension`（200–8192，默认 1600）；裁剪必须同时给绝对 `x/y/width/height`，且完整位于选中显示器；返回 PNG 原生 MCP image，编码不超过 16 MiB，不包含光标 |
| `window_focus` | `screen_id`、`window_id`；核验进程身份、窗口位置和权限，遵守 Windows 前台切换限制；失败如实返回 |
| `desktop_click` | `screen_id`、`x/y`；可选 `button: left/right/middle`、`clicks: 1/2` |
| `desktop_drag` | `screen_id`、`x/y/to_x/to_y`；可选 `button`、50–1000 ms 的 `duration_ms`；分步检查暂停、前台和期限，最终释放按键 |
| `desktop_scroll` | `screen_id`、`x/y`、非零 `delta`（-1200–1200）；可选 `horizontal` |
| `desktop_type` | `screen_id`、UTF-8 `text`，最多 512 个 UTF-16 单元和 4096 字节；直接 Unicode 输入，不使用剪贴板粘贴 |
| `desktop_key` | `screen_id`、1–4 项 `keys`；大写字母/数字、F1–F12、CTRL/ALT/SHIFT、ENTER/TAB/ESC/BACKSPACE/DELETE/HOME/END/PAGEUP/PAGEDOWN/UP/DOWN/LEFT/RIGHT/SPACE；修饰键在前、普通键在末，按下和释放成对提交 |
| `clipboard_read` | 独立读取权限；`lease_id`、`request_key`、观察中的 `expected_sequence`；仅 CF_UNICODETEXT，最多 1 MiB 原始 UTF-16 内存 |
| `clipboard_write` | 独立写入权限；上述序号及 UTF-8 `text`（最多 524287 字节）；序号改变则拒绝；成功写入会替换原剪贴板全部格式，返回新序号 |

点击、滚动及拖动端点必须位于观测的前台窗口，截图观察还要求坐标在截取区域内。已按住修饰键或鼠标键时拒绝新输入。键盘工具不支持 Win 键、Ctrl+Alt+Delete、自身紧急停止组合或保持按键。窗口焦点权限仅覆盖同账户、同会话且不高于普通主机完整性的目标。

`events_inserted` 表示 Windows 接收的事件数，`application_success_confirmed: false` 明确要求后续观察；事件提交成功不代表控件接受、内容保存或业务完成。多个进程的输入无法组成原子的应用事务；少量已提交事件不能撤销，部分失败仍会尝试释放自己按下的按键。[微软 SendInput 限制](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendinput)

桌面工具复用既有每项目最多 64 条审批/操作记录上限；达到上限停止新操作。先查看回执、释放控制，再由本机所有者明确重启项目；新实例重新取得控制，不重放旧输入。屏幕图片、窗口标题、输入文本、剪贴板正文和审批参数仅保留在当前进程内存，持久历史保存操作摘要、状态和已脱敏的结果元数据。截图不是录像，不持续保存屏幕。

## 暂停与本地恢复

取得控制期间，真实人工鼠标/键盘输入和其他进程注入的输入都会暂停整个会话。低级钩子只判断来源并更新暂停状态，不保存按键内容。`desktop_status` 中的来源计数用于核对暂停原因。

默认按 **Ctrl+Alt+F9** 紧急停止。正式界面只保留管理程序的一个托盘图标，项目不再单独注册图标。右键该图标，打开“桌面控制（本会话）”，可查看状态、暂停全部桌面操作、确认恢复，或进入已启用桌面控制的项目。这里的暂停/恢复影响本次 Windows 登录会话内的全部 ShellBot 项目，不是单个项目的启停。

托盘恢复等待 500 ms，等待期间有新的本机输入会取消恢复；恢复使旧观察失效，不自动重跑动作。管理器存活期间，暂停不会因所有项目停止或工作进程重启而消失。关闭管理窗口只收进托盘，项目和任务继续运行；从管理托盘选择“退出 ShellBot（停止全部项目）”才停止本实例全部项目、任务和网关。

本机所有者也可在普通终端使用：

```powershell
.\WinShellBot.exe --desktop-status --manager-data-dir 'D:\ShellBot\Data\Manager'
.\WinShellBot.exe --desktop-pause --manager-data-dir 'D:\ShellBot\Data\Manager'
.\WinShellBot.exe --desktop-resume --manager-data-dir 'D:\ShellBot\Data\Manager'
```

恢复不会自动把其他窗口切到前台。此命令只走同账户受保护本机管理通道，MCP 和远程所有者 API 都没有恢复入口。本版没有 UI Automation 控件树、应用语义操作、浏览器后端、OCR 或管理员桌面输入；这些属于后续扩展。

## 验证范围

本机专项使用专用 Win32 窗口核对真实像素、Unicode 输入、组合键、点击、拖动、滚动、前台切换、陈旧观察拒绝、跨项目和跨管理器互斥、租约过期、暂停及进程退出清理。剪贴板写入测试先在内存复制全部原格式并恢复，报告不保存正文。模拟外部输入和实际注册热键事件属于自动检查，不替代人工操作。

真实物理停止键、托盘菜单、锁屏/UAC/RDP 会话变化、不同缩放的多显示器、独立系统和真实 MCP 客户端仍须分别验收；本轮不触发 UAC 或进入安全桌面。最终结果、摘要及未覆盖项目见 [M8 交接](HANDOFF_20261011_M8.md)。测试源码和脚本继续由 `.gitignore` 排除，仅开发机保留。
