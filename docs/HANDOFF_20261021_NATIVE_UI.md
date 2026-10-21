# ShellBot M20 / dev.27 原生管理界面交接

日期：2026-10-21，北京时间。前序入口为 `HANDOFF_20261021_ARCHIVE.md`。本轮按用户新需求检查刷新和绘制逻辑、修正缺陷，并完成供后续正式版沿用的 Win32 管理界面。当前版本仍为开发候选，不将新界面等同于正式公开发行或真实客户端验收。

## 开发状态

- 工作目录：`E:\Program\ShellBot`，分支 `main`，HEAD `5acbc24437d109c9f0815baa75ca5c41fd7e9c0e`。
- 保留原有 M14–M19 未提交工作树；本轮也未提交、未推送。没有 reset、clean、覆盖真实 Data、重新授权 Z620 或修改系统网络设置。
- 变更前备份：`build\baseline-native-ui-20261021-160725\manifest.json`，包含本轮修改的既有源文件/版本断言和 Release 四组件。README 旧内容另与 M19 冻结摘要核对后保留。
- dev.26 和 dev.25 ZIP、M19 冻结记录及此前失败记录保持原样。当前源码和测试集合已变化，不重写 M19 收据来适配新集合。

## 界面和逻辑

管理界面继续使用 C17、Win32 和系统 Common Controls。没有引入新的运行时。新界面直接接入 `ShellBot.exe` 原有默认启动路径，涵盖系统菜单、全局管理操作、项目操作、项目列表/详情与底部状态提示。快捷键为 F5、Ctrl+N、Ctrl+E 和 Ctrl+L。窗口关闭收进托盘，文件菜单和托盘均可明确退出。

主要修正：异步诊断绑定原项目及选择代数；短暂刷新失败保留内容并标记过期状态；模态确认期间不发起后台刷新；合并选择通知；异步工作项在关闭时统一回收；OAuth 后台轮询不切换稳定按钮和列表，保留连接选择与阅读位置；保存线程启动失败恢复控件；连接字段统一冻结；复制失败不再提示成功；弹窗共享系统字体、DPI 布局与退出消息处理。嵌入的权限窗口不再显示独立配置的“保存到新文件”提示。补全默认按钮和 Enter/Escape 行为，授权页默认 Enter 关闭，不隐式同意连接。

逐项问题、修正和证据类别见 `UI_REVIEW_20261021.md`，日常流程见 `USER_GUIDE.md`。

主要入口：

| 文件 | 本轮作用 |
| --- | --- |
| `WinShellBot/platform/manager_ui.c` | 原生主界面、菜单、响应式分组布局、刷新状态、选择和异步结果绑定 |
| `WinShellBot/platform/native_ui.h` | 原生弹窗系统字体、DPI 同步、模态消息循环 |
| `WinShellBot/platform/desktop_dialog.c` | 项目/连接/存储弹窗的控件恢复和 DPI |
| `WinShellBot/platform/oauth_ui.c` | 授权列表增量更新、后台操作排队及阅读位置 |
| `WinShellBot/platform/project_settings.c` | 权限/目录/高级配置的 DPI 和嵌入界面提示 |
| `scripts/validate-native-ui.ps1` | 独立 M20 验证；旧报告先保留再执行 |
| `scripts/package-native-ui.ps1` | 按当前程序、报告与源/测试摘要打包独立 UI 候选 |
| `scripts/verify-native-ui-package.ps1` | 校验实际 ZIP 解压文件，验证原生窗口/弹窗与公开启动入口 |

## 验证与程序

Debug 和 Release 的 x64 构建通过，继续使用警告视为错误的现有工程配置。`build\native-ui-validation\acceptance.json` 汇总最终程序的 **6 份 Debug、13 份 Release** 报告，完整报告仍留在该目录。

- 两配置：确定性延迟/故障界面、实际原生弹窗、真实管理器刷新、OAuth 控件、OAuth 轮询事件和完整 OAuth 回归。
- Release 另验：真实桌面项目流程、公开启动入口与单实例/托盘、迁移取消/退出、独立权限配置、普通项目启停和任务、便携迁移、完整核心回归。
- `native_ui_checks.py` 每配置 274 项，以真实产品窗口和合成 owner 管理通道验证故障与竞态。不能把该合成通道当作真实管理器；真实管理器由其他报告单独覆盖。
- `native_ui_dialog_checks.py` 每配置 83 项，重复字体/DPI 消息后保留编辑内容，GDI 数量有界；实际 Enter 保存独立项目，Escape 取消项目/连接/存储/权限窗口。测试没有改变系统 DPI，物理跨屏切换仍待验。
- OAuth Release 实际等待授权码到期；完整核心回归包括 HTTP 和 100 次关闭探针。本轮未重跑 M19 的整套 4400 次严格 EOF 压力，不借用旧程序摘要作为新程序的压力证明。

最终主程序摘要：

| 组件 | SHA-256 |
| --- | --- |
| Debug WinShellBot.exe | `736c6d80019d0ce9460c283a0256aa9734cc34b7ac9e01bb79e8962b43bf73cd` |
| Release WinShellBot.exe | `8c9af0bcf0b55bafc11d70bda0f666740eabeb643f7f5091e3bc66c6f5262b87` |
| Release ShellBot.exe | `066b8bc2112118e355a4daadbd70bcd56c57f225ae76be588c572906c642692f` |

版本断言同步到本机 `tests/smoke.py`、`image_checks.py`、`desktop_app_checks.py` 和 `oauth_checks.py`。测试目录继续被 Git 忽略；需保留本机测试源和报告，不能认为单独克隆当前 HEAD 包含完整成果。

原始失败保留：受限编译环境的 PDB 工具版本错误记录在 `build-debug.log`，本机工具链构建通过；第一次新增弹窗测试在窗口显示前取样，修正等待可见条件后通过，原失败 JSON/日志在 `preserved-*` 中。先前通过报告对应不同的程序摘要，不混用于最终程序。`source-manifest.json` 与 `local-test-manifest.json` 记录打包时的准确集合。

## 候选包与运行边界

新包以 `ShellBot-0.1.0-dev.27-windows-x64-native-ui-test-*.zip` 独立保存到 `dist`，准确路径、摘要和解压检查见 `build\native-ui-validation\package-receipt.json`、`extracted-runtime.json`。包入口仍是 `ShellBot.exe`，含相邻运行四组件、使用说明、界面检查、许可证与摘要清单，不含 Data、凭据、位置设置、开发测试程序或脚本。

升级前托盘退出旧版，备份原 Data 和 `ShellBot.storage.json`；在原固定非 C 盘程序目录替换运行文件，保留数据、位置设置及 Windows 账户。OAuth 继续使用既有 v3 格式，本轮没有改变授权协议或项目策略，不需要为了新界面清空客户端授权。dev.26 回退可读取相同 v3 数据；若考虑更早版本，仍遵守 M19 对旧版不支持 v3 的边界。

AdGuard 服务和 localhost 过滤保持用户现有配置，不添加或删除排除。网络回归通过固定已排除的 Release/Debug 路径和专用测试客户端执行。解压后仅重复原生窗口/启动入口等适合未排除路径的项目，不宣称未排除路径已通过严格 EOF。

真实 MCP 客户端续期和长期运行、实际 UAC、物理多显示器 DPI、独立 Windows 系统仍待专项验收。已确认的旧人工项目不重置；真实命令和远端权限不在本轮范围。

## 后续继续

优先从本轮交接、界面审查和当前源码继续，保留所有基线、失败、报告和候选。若继续调整界面，先围绕用户实际使用反馈做增量修改；不要重新扩大到隧道、系统代理、真实客户端授权或清理真实数据。

构建和回归使用 `scripts/build.ps1 -Configuration Debug|Release -WithTests` 与 `scripts/validate-native-ui.ps1 -Configuration Debug|Release`。在合适的本机交互权限下执行，TEMP/TMP 仍放本轮 E 盘目录。新的产品变更需要新程序摘要对应的报告；不要仅重用版本号相同的旧结果。
