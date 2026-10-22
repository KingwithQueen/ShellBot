# ShellBot 1.0.2

面向 Windows 的原生 C17 / Win32 MCP 主机。双击 **ShellBot.exe**，在管理界面创建项目、设置权限、连接客户端并管理运行状态。

沿用第一个正式版的项目优先排版：左侧选择项目，右侧查看详情与启停，顶部设置连接、授权和数据位置。只保留管理程序一个托盘图标，桌面控制与项目入口集中在托盘菜单。可从“视图”菜单切换经典布局；自动刷新保留阅读位置和诊断内容。

## 开始使用

1. 完整解压 Windows x64 发布包到可写的非 C 盘目录，保留全部组件和许可证。
2. 双击 **ShellBot.exe**，点击“新建项目…”，选择工作目录并设置所需权限。
3. 在“连接设置…”填写本机监听端口和已有 HTTPS 公共地址。程序只监听 `127.0.0.1`；隧道和域名使用已有配置。
4. 在 MCP 客户端添加项目公网地址并选择 OAuth。在 ShellBot 的“客户端授权…”中核对申请，再允许连接。

关闭窗口会收进托盘；从“文件”或托盘菜单退出会停止全部项目。详细操作见 [使用说明](docs/USER_GUIDE.md)，本版变化与验证范围见 [1.0.2 发布说明](docs/RELEASE_NOTES_1.0.2.md)。

## 数据与升级

默认数据保存在程序旁的 `Data`。可以通过“数据位置…”迁移到其他非 C 盘目录；复制和逐文件校验成功后才保存新位置，原目录由用户确认后清理。`ShellBot.storage.json` 保存自选位置，应与程序一起保留。凭据由当前 Windows 账户 DPAPI 加密。

升级时先从托盘退出旧版，备份 Data 和位置设置，再替换同一固定程序目录中的发布文件。保留原数据、位置设置和 Windows 账户。1.0.2 沿用 1.0.0–1.0.1 和 dev.26–dev.28 的 OAuth v3 存储，既有长期授权继续使用；旧限时连接不会自动升级。不要用发布包覆盖或删除 Data。

## 主要能力

| 能力 | 说明 |
| --- | --- |
| 项目管理 | 原生窗口、项目独立凭据与工作进程、启停与暂停、目录迁移、托盘生命周期 |
| 连接认证 | OAuth 发现、公共 DCR、授权码/S256 PKCE、轮换续期、长期或限时授权、手动撤销 |
| 文件 | 列表、读取、搜索、创建、基于摘要的修改、复制、移动、删除、独立审批与审计 |
| 命令任务 | 普通、受限及管理员身份，后台输入输出、取消与超时；按项目明确授权 |
| 图像与文档 | 图像解码、缩放；PDF 阅读与渲染；DOCX / XLSX / PPTX 结构化阅读及内容预览 |
| 桌面与传输 | 独立授权的 Windows 桌面操作，产物下载及校验后创建文件的上传 |

共实现 37 个工具，各项目只暴露其策略允许的工具。文件范围、审批方式、执行身份和管理员许可独立设置。受限执行只开放声明的运行文件；普通或管理员命令具有相应账户权限。Office 预览不复现原页面排版，扫描文档不自动 OCR。

更多说明：[文件管理](docs/FILE_MANAGEMENT.md)、[权限与身份](docs/EXECUTION_PERMISSIONS.md)、[命令任务](docs/COMMAND_TASKS.md)、[远程审批](docs/REMOTE_APPROVALS.md)、[桌面控制](docs/DESKTOP_CONTROL.md)、[文档阅读](docs/DOCUMENT_READING.md)、[产物传输](docs/ARTIFACT_TRANSFER.md)。

## 构建与发布

安装 Visual Studio C++ 桌面开发工具及 Windows SDK，在仓库目录运行：

```powershell
.\scripts\build.ps1 -Configuration Debug
.\scripts\build.ps1 -Configuration Release
```

自有代码使用 C17、四级警告及警告视为错误，Release 使用静态运行库。默认构建同时生成 GUI 入口、主程序、SandboxProbe 并复制固定 PDFium 依赖。当前正式包为 Windows x64；未将其他系统或架构列为已验收支持。

本机测试源与历史证据保留在被 Git 忽略的 `tests`、`build`、`out`、`dist` 中。恢复本机测试副本后可用 `-WithTests` 构建原生夹具；当前验证收据位于 `build/release-1.0.2`。首次正式版记录位于 `build/release-1.0.0`，单托盘调整见 `build/single-tray-1.0.0` 和 [单托盘说明](docs/SINGLE_TRAY_1.0.0.md)。发布包与当前报告通过 `scripts/package-release.ps1` 绑定版本、程序、源码及测试摘要。历史报告和原始失败记录仍保留，过时的候选程序及压缩包按清理收据移除。

本地自动化与真实客户端、实际 UAC、物理多屏 DPI、独立 Windows 环境、长期运行分别验收，准确范围见发布说明与包内验证摘要。

## 许可

Copyright (c) 2026 姬敬。项目使用 [MIT 许可证](LICENSE.txt)；[第三方组件](third_party/README.md)保留各自许可，发布包的 `licenses` 目录包含完整声明。
