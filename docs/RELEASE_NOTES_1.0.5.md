# ShellBot 1.0.5

Windows 正式版。发布文件固定命名 `ShellBot-1.0.5-windows-x64.zip`；后续优化直接递增版本号，不添加测试标签、阶段名或日期后缀。

## 参数超限提示

容量检查返回具体字段、实际值、上限和单位，同时提供 `code: parameter_limit_exceeded`、`field`、`actual`、`maximum`、`unit`。例如请求输出页超过 1 MiB：

```json
{"code":"parameter_limit_exceeded","field":"max_bytes","actual":1048577,"maximum":1048576,"unit":"bytes","error":"max_bytes: actual=1048577 bytes, maximum=1048576 bytes."}
```

覆盖文本、分块、参数数组、请求键、读取页和数值参数；Windows 命令行会报告转义后的总长度或超长 argv 下标，单位为 UTF-16 单元。HTTP 请求正文过大也返回实际字节数与上限。正文内容不进入诊断信息。错误类型、非法路径、权限拒绝和编码错误仍各自报告，不冒充长度超限。

## 中文任务日志

`task_output` 新增可选 `encoding`。支持 `utf-8`、`cp936` / `gbk`，以及 Windows 支持的 `cpNNNN` 严格解码。返回的 `text` 是可直接显示的 Unicode 字符串，`text_encoding` 标明选择的源编码。升级后重新连接 MCP 客户端，刷新工具定义后即可提交新参数。

原始 `data_base64`、`encoding:base64`、字节 `next_offset` 和默认行为保留。指定编码时另给 `text_next_offset`，用于续读跨页的完整字符；`decode_status:partial` 表示页尾有未完整字符，应使用文本游标继续。编码无效或末尾不完整时标注 `invalid_or_incomplete` 并保留原始字节，不悄悄插入替代字符。详细用法见 [命令任务](COMMAND_TASKS.md)。

## 更新与验证

正式包包含 `ShellBotUpdate.exe`，支持从本地 ZIP 更新并保留 Data、位置设置和备份，使用方法见 [更新说明](UPDATING.md)。1.0.4 提高的数据容量继续有效。

本版验证范围为 Windows Debug/Release 构建、新功能专项、后台文件/媒体/任务/审批/网关回归，以及隔离安装中的更新器回归和解压后的运行验证。结果与当前程序摘要绑定。1.0.4 的用户实测与本轮自动化验证分别记录；本轮未重跑实体桌面、多屏、实际管理员/UAC、重启登录及外部客户端全链路。正式版身份不表示这些独立环境均重新验收。

升级前退出旧版，保留 Data、`ShellBot.storage.json` 及 Windows 账户。Mac/Linux 的重新移植继续单独安排。
