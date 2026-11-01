# ShellBot 1.1.3 Windows

正式包：`ShellBot-1.1.3-windows-x64.zip`。升级后重新连接 MCP 客户端刷新工具定义，原项目不自动开放 UIA 工具。

- ConPTY：task_start 可选 io_mode:terminal；继续用 task_input/output/status/cancel/list/archive 管理真实交互终端，支持中文、控制序列、幂等输入与队列内调尺寸。默认管道行为保留；终端采用合并 UTF-8 VT，不能用它替代逐字节 stdout/stderr 编译日志。
- 模式搜索：fs_search 增加 pattern_type:literal/regex/glob、ignore_case、file_glob。静态 PCRE2 10.49 Unicode 匹配，保留逐文件范围检查，模式无效与预算耗尽分别报告。查询上限提高到 16 KiB。
- UIA：新增 uia_tree、uia_action，支持控件树、显式非密码文本和六类语义动作；沿用桌面权限、调用方租约、观察/暂停规则。辅助进程隔离无响应提供者，派发与实际结果分开，超时不自动重放。

详细参数、示例、预算和适用范围见 [交互工具](INTERACTIVE_TOOLS.md)。[Harness 对照](HARNESS_TOOL_COMPARISON.md) 保留原固定提交，并记录本版已补齐的部分能力。

验证包含真实私有 ConPTY、中文/尺寸/中断/退出/取消/超时、大量 VT 输出关闭、工作区 glob/正则、只读 UIA 自有屏幕外窗口、拦截 UIA 动作 API 的边界检查和辅助进程异常回收。实体应用动作、真实 mstsc 最小化、UAC/物理输入、外部客户端及 Mac/Linux 原生版本仍需独立验收；本版只完善 Windows。

完整回归中的 Debug 历史批量任务检查有一次工作进程回复超时；同一可执行文件随后两次独立完整复测通过，准确原因尚未确认。保留首次失败及两次复测的独立记录，包内验证摘要列明，没有通过增加超时或自动重放掩盖该情况。
