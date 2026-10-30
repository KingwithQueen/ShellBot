# ShellBot 1.0.7

Windows 正式版，发布包命名为 `ShellBot-1.0.7-windows-x64.zip`。升级后重新连接 MCP 客户端，刷新 `desktop_key` 的工具说明和参数定义。包含本地 ZIP 更新器，保留原 Data、位置设置和 Windows 账户。

- 为 `desktop_key` 增加显式 `key`、`modifiers` 参数，直接表达 Enter、Shift+Enter、Ctrl+V 等单次按键或组合键。旧 `keys` 数组继续支持；名称不区分大小写，支持 Control、Return、Escape 别名。
- 工具发现、Unicode 文本输入和剪贴板写入说明明确指向按键接口，区分文本换行、实际 Enter 和粘贴。
- 回执报告规范化按键、事件提交数、部分提交后的释放结果及原始 Windows 错误。部分提交只释放本次已按下且尚未释放的键；完整结果仍需调用方观察应用确认。

调用示例和边界见 [桌面控制](DESKTOP_CONTROL.md)。本轮聚焦 Windows，未重新移植 Mac/Linux。

验证包含真实 MCP 工具发现与参数拒绝、原生事件序列和部分提交错误注入，以及既有后台回归。专用输入框的真实按键验证在取得初始焦点时被 Windows 拒绝，未发送输入；因此不宣称已完成目标应用中的 Enter、Shift+Enter 或 Ctrl+V 实机验收。实际 UAC、外部客户端和独立设备未在本轮重跑，准确范围见随包 `validation-summary.json`。

更新器回归曾因临时 DLL 占用而保留部分缓存，严格清理检查未通过；同构建单独复测通过，原始记录保留，相关测试缓存已清理。
