# ShellBot 1.0.0 正式版交接

日期：2026-10-21，北京时间。用户已采用 dev.28 的项目优先排版作为第一个正式版，并要求改为 **1.0.0**、清理旧的无用文件。

## 当前实现

- 保留已采用的原生 C17 / Win32 项目优先界面及“视图 → 经典布局”。本轮没有再次调整布局或改变权限、授权、数据格式。
- `core/include/sb_version.h` 统一版本 `1.0.0`、Windows 数字版本 `1,0,0,0` 和版权信息。`sb.h` 引入此头文件，旧脚本若直接从 sb.h 搜索版本宏，需要改读新头文件。
- `WinShellBot/app/version.rc` 为主程序、GUI 入口和 SandboxProbe 提供 Windows 产品/文件版本资源。Windows 属性、CLI、关于窗口、MCP 使用相同版本。
- 用户确认版权署名 **2026 姬敬**；MIT 正文不变，PDFium 等第三方许可完整保留。
- README、使用说明和发布说明改为正式版流程；开发前的完整文档在本轮基线和 Git 历史中。专项历史交接和原始失败报告保持原样。
- `scripts/package-release.ps1` 是当前正式打包入口；旧里程碑脚本属于历史，不要用其硬编码版本或旧 finalizer 覆盖冻结记录。

## 产物与本机验证

正式包：`dist/ShellBot-1.0.0-windows-x64.zip`，旁置 `.zip.sha256`。入口 **ShellBot.exe**，4 个运行组件、当前操作文档、完整第三方声明和验证摘要；不包含 Data、位置设置、私有配置、凭据或开发夹具。

Release 运行组件摘要：

| 组件 | SHA-256 |
| --- | --- |
| WinShellBot.exe | `6d8f024c3f6ad021799f1a7403eedf4880316e21a0dd900cba1a7afd255cea35` |
| ShellBot.exe | `b2adf3a8aaba5a089a9581a1fb76ec5ad50ae828a919c4ab028bee21e7fba993` |
| ShellBot-SandboxProbe.exe | `f5493316e0e6459cd96794dbce4966d792df1eccc402693ae890ec3f943e8cfd` |
| pdfium.dll | `1f2aee4286b8cf0413ab4562b7121aa454f9cfd9afd6ca8609bcef80927b3f80` |

本轮全部记录位于 `build/release-1.0.0`：

- Debug / Release 构建完成，无编译警告；当前程序绑定的 25 份本机报告全部通过。报告计数合计 11241，包含 400 次严格 EOF 请求，计数不能替代各报告实际范围。
- Debug：完整核心、两种布局、刷新、原生界面、OAuth。Release 另含原生表单、桌面生命周期、OAuth 控件、便携迁移、文件/设置、图像、文档/后端、传输、项目策略和网关。OAuth Release 实际等待授权码到期。
- `*-binding.json` 绑定报告、程序和测试脚本摘要；`package-gates.json` 记录缺失、旧摘要、排除核心以及失败子项四类拒绝门禁。
- `package-receipt.json`、`acceptance.json` 和源码/本机测试清单为最终产物入口；ZIP 摘要以旁置校验文件和收据为准。
- `extracted-runtime.json` 单列实际解压的两种界面与 GUI 入口流程，核对完整文件集、全部摘要及结束时所属进程为零。固定排除路径的网络检查不冒充未排除解压路径的 TCP 验收。

AdGuard 服务和 localhost 过滤保持，沿用四个固定程序路径排除。未改隧道、真实项目、权限或连接凭据。测试只使用独立数据；本轮给原生表单检查补入空索引/独立端口准备，避免默认端口与用户实例冲突。

## 清理与恢复边界

变更前基线：`build/baseline-release-1.0.0-20261021-212426/manifest.json`，包括原源码、文档、测试和两个配置的四组件；第三方说明另补入同一清单。

清理只允许：7 个 dev.22–dev.26 旧解压目录中与原 ZIP 全集逐字节一致的副本，以及 `build/x64` 下已枚举的 MSBuild 缓存。先检查路径、重解析点、运行进程、文件全集和摘要，缓存压缩恢复副本验证成功后才删除。实际删除数量、空间和恢复位置以 `build/release-1.0.0/cleanup-receipt.json` 为准。

旧 ZIP 和校验文件全部保留。包含 Data 的 dev.27 目录、dev.28 布局回退包、基线、原始报告、源码/测试、out 运行组件及 `build/tools/ShellBotNetworkTest/ShellBot-TestClient.exe` 均不属于清理对象。没有使用 git clean/reset，没有清理原始失败。

恢复旧解压副本可将收据列出的原 ZIP 解压至 dist；编译缓存可直接重建，或把恢复目录中的 msbuild-cache.zip 解压到 build/x64。不要把数据目录当缓存恢复覆盖。

## 升级与未覆盖范围

退出旧版，备份 Data 和 ShellBot.storage.json，再替换同一固定非 C 盘目录中的运行组件；保留原数据、位置设置和 Windows 账户。1.0.0 继续使用 OAuth v3，旧限时连接不会自动升级为长期连接。

真实 MCP 客户端在本版的持续续期、实际 UAC、物理多屏 DPI、独立 Windows 系统和长期运行尚未由本轮自动化验收。当前正式版选择是用户的产品决定，不把历史人工结果或本机测试改写成这些验收通过。

开发基准：main，HEAD `9035f512dfec9e75ad5c8c74368960319f23aaa2`。本轮改动未自动提交、打标签或推送；测试、build/out/dist 为本机忽略内容。继续时先核对工作区和最终收据，不重跑旧阶段 finalizer。
