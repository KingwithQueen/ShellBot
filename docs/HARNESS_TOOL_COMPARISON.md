# DeepSeek Harness 工具对照（2026-11-02 更新）

核对对象为 DeepSeek 官方仓库 [deepseek-ai/deepseek-harness](https://github.com/deepseek-ai/deepseek-harness)，固定提交 [`ce7073d51254c26cd63b22a60835b8f948deb447`](https://github.com/deepseek-ai/deepseek-harness/tree/ce7073d51254c26cd63b22a60835b8f948deb447)。对照 ShellBot Windows 1.1.3；上游来源仍固定在该提交，不宣称这是上游最新版本。

本表检查仓库中的工具注册源码及电脑/浏览器适配器，不把“仓库里存在”当作“默认全部启用”。DSH 的工具可由插件选择、改名、条件启用；外部 MCP/CUA 适配器还会动态导入上游工具。本表不声称枚举任意用户安装的第三方插件。

## 能借鉴的设计

1. **电脑操作按能力接入。** [computer-use 子系统](https://github.com/deepseek-ai/deepseek-harness/blob/ce7073d51254c26cd63b22a60835b8f948deb447/docs/subsystems/computer-use.md) 将能力登记和具体实现分开，不强行给不同平台声明同一组可用操作。ShellBot 保留 Windows 原生实现，本次通过实际工具 schema 和 host_capabilities 声明窗口恢复能力。
2. **派发与成功分开。** [CUA 对象 API](https://github.com/deepseek-ai/deepseek-harness/blob/ce7073d51254c26cd63b22a60835b8f948deb447/packages/experimental/cua-macos/src/guidance.ts) 的显示/激活返回值区分已派发与未验证；这是 macOS 提供者的设计，不能当作 Windows 实现。ShellBot 1.1.2 分开报告进程创建、窗口恢复、真实前台观察和截图，启动成功不再被解释为已聚焦。
3. **先观察、操作后核实，取消后不盲目重放。** [原生 CUA Driver 适配器](https://github.com/deepseek-ai/deepseek-harness/blob/ce7073d51254c26cd63b22a60835b8f948deb447/packages/experimental/computer-use-cua-driver-native/src/index.ts) 有这些调用约束。ShellBot 延续自己的租约/观察编号，并使恢复和聚焦后的旧观察失效；没有把桌面操作改为并行执行。
4. **搜索和终端有独立契约。** [搜索工具](https://github.com/deepseek-ai/deepseek-harness/blob/ce7073d51254c26cd63b22a60835b8f948deb447/packages/fs/tool-fs-search/src/index.ts) 用固定参数调用随包的 ripgrep，而非拼接 shell；[PTY 工具](https://github.com/deepseek-ai/deepseek-harness/blob/ce7073d51254c26cd63b22a60835b8f948deb447/packages/experimental/tool-terminal/src/index.ts) 把交互终端与普通后台进程分开。ShellBot 1.1.3 为搜索接入静态 PCRE2 和 glob，并给原任务增加真正的 ConPTY 模式；这是独立实现，没有复制上游代码。

未发现被核对的 DSH 适配器自行实现 Windows RDP 最小化的通用前台修复。CUA Driver native/MCP 提供者转交上游驱动，不能从适配器代码推断其在某台 RDP 机器上的表现。本轮没有复制其代码、安装或执行其依赖。

## 工具与能力差异

| DSH 工具/能力 | ShellBot 对应情况 | 缺口和适用位置 |
| --- | --- | --- |
| `read`、`read_image`、`write`、`edit`、`str_replace_editor` | `fs_read`、`image_view`、`fs_write`、`fs_patch` 已有主体能力 | 按行插入等编辑形式不同；不能列为整类缺失 |
| `apply_patch`（工具包内可选启用） | `fs_patch` 是文字查找替换 | 缺少多文件统一补丁工具及其路径/冲突/审批契约 |
| `glob`、`grep` | `fs_search` 支持 literal/regex/glob、ignore_case、file_glob | 1.1.3 已补模式搜索；使用 PCRE2，并非复制 ripgrep 的所有选项/忽略文件语义 |
| `bash`、`pwsh`，持久 shell 提供者 | `task_start/input/output` 可运行对应程序 | 没有一等持久 shell 会话及逐条命令完成边界；不等于不能运行脚本 |
| `terminal_open/send/read/signal/close/list`（实验） | task_start io_mode:terminal、task_input/output/status/cancel/list/archive | 1.1.3 Windows 已有真实 ConPTY、控制序列和排队调尺寸；没有照搬独立终端工具名，也不声称支持全部 POSIX 信号 |
| `job_output/list/kill` | `task_output/list/cancel`，另有 `task_status/archive` | 有对应能力，不应重复新增同义工具 |
| 浏览器 MCP 提供者（Playwright、Chrome DevTools 等） | 只有桌面键鼠/截图路径 | 缺少浏览器标签、DOM、页面状态等结构化协议；实际名字以启用提供者目录为准 |
| CUA 元素树、元素操作、文本选区、语义动作 | 1.1.3 Windows 新增 uia_tree/action、非密码文本及六类 pattern | 已补 Windows UIA 基础，文本选区和任意语义动作仍未实现；上游 macOS CUA 不能等同于其 Windows 实现 |
| `web_search`、`web_fetch` | 无一等网页搜索/获取工具 | 可通过已授权命令运行网络程序，但没有同等搜索提供者、来源及结果结构；搜索服务和凭据适合可选接入 |
| `lsp` | 文件编辑/搜索已有 | 缺少语言服务诊断、符号/定义等语义查询 |
| `create_worktree`（实验） | 可通过 task_start 调用 Git | 无一等工作树生命周期管理；可以先留给调用端 |
| `skill`、`load_workspace_dependencies` | 可以读技能文件、执行已安装程序 | 无技能登记/按需加载、工作区依赖材料化协议；不是原生文件工具的同义项 |
| `present` | `artifact_download/upload` 已支持传输 | 缺少客户端侧展示/引用协议；须由客户端配合，不能只加一个服务器接口 |
| `ask_user_question` | 有所有者审批 | 无通用结构化澄清提问；审批不等于任意问答，宜由模型客户端承担 |
| `get_goal/create_goal/update_goal`、`todo_write` | 无 Agent 目标/待办运行时 | 属于完整 Agent 平台；不建议直接放进本地执行服务器 |
| `subagent`、`send_message`、`interrupt_agent`、实验团队工具 | 无子代理编排 | 适合调用端/独立 Agent 层，不是桌面聚焦修复的依赖 |
| `schedule_create/list/view/edit` | 有主机程序自启，没有 Agent 计划任务工具 | 缺少对话/Agent 调度；与 Windows 登录自启不同 |
| `workflow`、实验 `ralph` / `subagent_oar` | 无 Agent 工作流运行时 | 属于编排层，不能与普通任务 Job 混同 |
| `session_search/event_search/trace/event_trace/event_read`（实验） | 有审计、请求诊断和任务结果 | 无完整对话历史检索运行时；请求诊断不等于对话检索 |
| `working_directory`、`cordis_inspect_list/query` | 工作区/path 显式传入；host_capabilities 提供主机状态 | 前者是会话默认目录、后者是 Cordis 插件内部检查；不是必须照搬的主机工具 |

文件类依据：[tool-fs](https://github.com/deepseek-ai/deepseek-harness/tree/ce7073d51254c26cd63b22a60835b8f948deb447/packages/fs/tool-fs)、[tool-fs-search](https://github.com/deepseek-ai/deepseek-harness/tree/ce7073d51254c26cd63b22a60835b8f948deb447/packages/fs/tool-fs-search)。其余注册入口位于仓库 `packages/<分类>/tool-*/src/index.ts`；`web` 工具在 `search.ts`/`fetch.ts` 注册，子代理/工作流部分名称可配置。电脑和浏览器适配器的动态目录另计。

1.1.3 已补 ConPTY、glob/正则和 Windows UIA 基础，契约与限制见 [交互工具](INTERACTIVE_TOOLS.md)。下一步仍需真实应用和 RDP 验收，再按需求扩展 UIA 文本选区、多文件补丁等。网页/浏览器接入需要独立提供者及权限设计。目标、团队、工作流、会话等 Agent 平台功能先保留在调用端。本轮只实现用户选定的三项能力，其余差异继续保留，不批量新增未设计的工具。
