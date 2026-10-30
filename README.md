# ShellBot 1.0.5

面向 Windows 的原生 C17 / Win32 MCP 主机。双击 **ShellBot.exe**，在管理界面创建项目、设置权限、连接客户端并管理运行状态。

沿用第一个正式版的项目优先排版：左侧选择项目，右侧查看详情与启停，顶部设置连接、授权和数据位置。只保留管理程序一个托盘图标，桌面控制与项目入口集中在托盘菜单。可从“视图”菜单切换经典布局；自动刷新保留阅读位置和诊断内容。

## 开始使用

1. 完整解压 Windows x64 发布包到可写的非 C 盘目录，保留全部组件和许可证。
2. 双击 **ShellBot.exe**，点击“新建项目…”，选择工作目录并设置所需权限。
3. 在“连接设置…”填写本机监听端口（1～65535）和已有 HTTPS 公共地址。程序只监听 `127.0.0.1`；隧道和域名使用已有配置。
4. 在 MCP 客户端添加项目公网地址并选择 OAuth。在 ShellBot 的“客户端授权…”中核对申请，再允许连接。

关闭窗口会收进托盘；从“文件”或托盘菜单退出会停止全部项目。详细操作见 [使用说明](docs/USER_GUIDE.md)，本版变化与验证范围见 [1.0.5 发布说明](docs/RELEASE_NOTES_1.0.5.md)。

## 数据与升级

本地 ZIP 更新程序见 [ShellBotUpdate 使用说明](docs/UPDATING.md)。将更新器放在安装根目录后选择下载好的发布包，更新前自动校验并备份，保留 Data 和位置设置。

默认数据保存在程序旁的 `Data`。可以通过“数据位置…”迁移到其他非 C 盘目录；复制和逐文件校验成功后才保存新位置，原目录由用户确认后清理。`ShellBot.storage.json` 保存自选位置，应与程序一起保留。凭据由当前 Windows 账户 DPAPI 加密。

升级时先从托盘退出旧版，备份 Data 和位置设置，再替换同一固定程序目录中的发布文件。保留原数据、位置设置和 Windows 账户。1.0.5 沿用 1.0.0–1.0.4 和 dev.26–dev.28 的 OAuth v3 存储，既有长期授权继续使用；旧限时连接不会自动升级。不要用发布包覆盖或删除 Data。

## 主要能力

| 能力 | 说明 |
| --- | --- |
| 项目管理 | 原生窗口、项目独立凭据与工作进程、启停与暂停、目录迁移、托盘生命周期 |
| 连接认证 | OAuth 发现、公共 DCR、授权码/S256 PKCE、轮换续期、长期或限时授权、手动撤销 |
| 文件 | 列表、读取、搜索、创建、基于摘要的修改、复制、移动、删除、独立审批与审计 |
| 命令任务 | 普通、受限及管理员身份，后台输入输出、取消与超时；按项目明确授权 |
| 图像与文档 | 图像解码、缩放；PDF 阅读与渲染；DOCX / XLSX / PPTX 结构化阅读及内容预览 |
| 桌面与传输 | 独立授权的 Windows 桌面操作，产物下载及校验后创建文件的上传 |

Windows 1.0.5 的源码编辑最高 8 MiB，文件分块传输最高 4 GiB，图像源最高 64 MiB；完整限制及大源码用法见 [数据容量](docs/CAPACITY_LIMITS.md)。

共实现 37 个工具，各项目只暴露其策略允许的工具。文件范围、审批方式、执行身份和管理员许可独立设置。受限执行只开放声明的运行文件；普通或管理员命令具有相应账户权限。Office 预览不复现原页面排版，扫描文档不自动 OCR。

更多说明：[文件管理](docs/FILE_MANAGEMENT.md)、[权限与身份](docs/EXECUTION_PERMISSIONS.md)、[命令任务](docs/COMMAND_TASKS.md)、[远程审批](docs/REMOTE_APPROVALS.md)、[桌面控制](docs/DESKTOP_CONTROL.md)、[文档阅读](docs/DOCUMENT_READING.md)、[产物传输](docs/ARTIFACT_TRANSFER.md)。

## 构建与发布

安装 Visual Studio C++ 桌面开发工具及 Windows SDK，在仓库目录运行：

```powershell
.\scripts\build.ps1 -Configuration Debug
.\scripts\build.ps1 -Configuration Release
```

自有代码使用 C17、四级警告及警告视为错误，Release 使用静态运行库。默认构建同时生成 GUI 入口、主程序、SandboxProbe 并复制固定 PDFium 依赖。当前正式包为 Windows x64；未将其他系统或架构列为已验收支持。

仓库保留源码、构建与正式打包脚本、示例配置、图标资源、依赖、许可证及产品文档。交接文档、AI 客户端状态与对话导出、阶段开发记录、本机验收脚本，以及 `tests`、`build`、`out`、`dist` 中的测试和产物只保留在本地，不随 Git 同步。新机器需要完整开发证据时应另行复制这些本地资料；默认构建不依赖它们，`-WithTests` 需要恢复本机测试副本。

1.0.5 为 Windows 正式版，发布包固定命名为 ShellBot-1.0.5-windows-x64.zip，包含 ShellBotUpdate.exe。参数超限诊断和指定编码日志读取见发布说明；当前构建的后台回归及验证收据位于 build/release-1.0.5。后续优化直接递增版本号。历史收据、安装包和原始失败记录保留，1.0.4 容量专项仍位于 build/limits-1.0.4。

本地自动化与真实客户端、实际 UAC、物理多屏 DPI、独立 Windows 环境、长期运行分别验收，准确范围见发布说明与包内验证摘要。

## Linux 和 macOS 后台开发版

独立 C17 POSIX 后端使用 CMake 构建，可在本机编辑源码后通过 SSH 到 Linux/macOS 原生编译测试。已提供基础文件、项目独立工作进程、普通命令、加密凭据与 OAuth，以及 Mac Cocoa / Linux GTK 4 原生管理窗口和托盘。窗口可管理项目、审批和连接，普通命令具有当前账户权限，工作目录不是命令沙箱。第五阶段增加屏幕查看、键鼠与文本剪贴板、本机连接/暂停及短时租约，参见 [Linux / Mac 远程桌面](docs/POSIX_REMOTE_DESKTOP.md)。Wayland 和 Mac 系统授权后的实机操作需另行验收。第六阶段增加产物传输、图片查看和 PDF/Office 读取，参见 [媒体与产物](docs/POSIX_MEDIA.md)。第七阶段增加登录自启和独立后台服务安装，参见 [自启与后台服务](docs/POSIX_STARTUP.md)。第八阶段增加原生受限命令及所有者显式授权的独立管理员命令代理，参见 [命令身份](docs/POSIX_COMMANDS.md)；真实 sudo、桌面及重启验收分别记录。第九阶段补齐完整项目配置表单、独立远程所有者审批、项目与应用数据迁移，参见 [项目配置与迁移](docs/POSIX_PROJECTS.md)。第十阶段补齐管理员文件、图片/文档和产物代理，以及 Intel Mac 的命令内存配额。当前目标为 Linux x86_64 / Intel Mac（macOS 13+），功能完成后集中人工验收；这是跨平台开发版。

构建、配置、目录边界及远端开发流程见 [Linux/macOS 后台说明](docs/POSIX_BACKEND.md)，原生窗口使用见 [管理界面与托盘](docs/POSIX_DESKTOP.md)，集中验收安排见 [Linux / Intel Mac 人工验收](docs/POSIX_ACCEPTANCE.md)。Windows 继续使用上面的 MSBuild 入口，原有 Win32 平台代码及发布包保持独立。

## 许可

Copyright (c) 2026 姬敬。项目使用 [MIT 许可证](LICENSE.txt)；[第三方组件](third_party/README.md)保留各自许可，发布包的 `licenses` 目录包含完整声明。
