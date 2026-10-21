# ShellBot 当前对话归档交接

日期：2026-10-21，北京时间。本文是归档当前对话后，新对话的首读入口。当前交付版本为 **M19 / 0.1.0-dev.26**；本机回归、打包和实际解压验证完成，真实客户端验收仍待进行。用户准备在新对话开发新内容，具体主题尚未指定，不要自行把下述待验收项当作必须立即执行的下一任务。

## 1. 开发位置与继续原则

- 工作目录：`E:\Program\ShellBot`。
- 分支：`main`；HEAD：`5acbc24437d109c9f0815baa75ca5c41fd7e9c0e`。
- M14–M19 的源码、脚本和文档包含大量未提交改动及未跟踪文件。**未提交、未推送**；交接时保留原状，不 reset、clean、覆盖工作树或自动提交/推送。
- `tests/`、`build/`、`out/`、`dist/` 被 Git 忽略。测试源、原始日志、基线和候选包均只在本机，单独克隆当前 HEAD 不会得到本轮完整实现与证据。
- 开始新开发前先读本文，再按目标阅读实际源码及相关专项交接。沿用现有 C17 / Win32 实现，先检查已有能力，不重新从旧 M12/M13 状态推导项目进度。
- 旧阶段的“暂缓 OAuth”“只收尾、不开发新功能”等范围已由后续明确需求覆盖；现在已经实现 OAuth 和长期授权。下一轮按用户新需求确定范围。

归档交接只新增本文和独立核对记录；不改已冻结 M19 收据、包内清单及专项交接。M19 冻结的源码清单有 179 个文件，新增本文后当前源码集合会增加。今后直接重跑旧 finalizer 的“源码集合一致”检查会因新增本文而不匹配，这是冻结后增加文档，不能通过重写旧收据消除。以原包、原始摘要及独立交接核对记录区分两个时点。

## 2. 用户已经确定的产品行为

1. 日常只运行 **ShellBot.exe**。管理界面直接新建项目，弹窗配置成功后进入列表；配置、连接、启停、数据位置和客户端授权均在界面完成，不再要求日常 CMD 脚本。
2. 管理界面持有管理器、网关和全部项目工作进程。关闭窗口收进托盘；托盘右键退出才结束程序，并停止全部项目及任务。
3. 便携版默认把应用配置、凭据、项目、日志、审计、产物、缓存和临时目录放在程序旁 `Data`。界面可以修改位置；先复制、逐文件 SHA-256 核对、再发布新位置，重启后由用户确认并手动删除原目录。失败保留原配置、原目录及可供检查的目标。
4. **ShellBot 自身数据不放 C 盘**，包括实际解析到 C 盘的目录别名。位置不明确时询问用户。该限制不禁止项目已经明确授权的 C 盘文件操作。
5. 数据位置设置在程序旁 `ShellBot.storage.json`；项目索引和账户加密凭据在 `Data\Desktop`，管理器/OAuth 状态在 `Data\Desktop\Manager`，默认项目在 `Data\Projects`。项目配置与凭据由当前 Windows 账户 DPAPI 加密，不能按普通文件跨账户使用。
6. 文件范围、操作审批、命令身份和管理员执行许可是独立策略。连接认证不会替代操作审批，也不自动获得管理员权限。
7. 用户维护已有隧道和域名。历史使用过 `https://shellbot.jsnowly.win` 映射本机 `8765`；后续应核对实际项目/界面，不能把该历史地址直接当成 Z620 当前配置并覆盖。ShellBot 监听 `127.0.0.1`；不自行扩展隧道、cloudflared 或域名管理。
8. 保留旧候选、基线、原始失败和通过报告。清理只针对已核验可重建或确实无用的对象，不能清除项目、凭据、恢复副本和原始证据。本次归档不清理、不重置数据。

## 3. 从旧交接到当前的完成内容

| 阶段 | 当前已经实现的主要结果 |
| --- | --- |
| M13 / dev.20 | 真实项目配置、文件操作、HTTP 回归及候选打包；后续 UI 需求已由 M14 替代日常启动方式。 |
| M14 / dev.21 | 单一 GUI 入口、新建/修改项目、项目目录复制迁移、托盘生命周期、退出停止全部项目。 |
| M15 / dev.22 | OAuth 发现、公共动态客户端注册、授权码与 S256 PKCE、访问/轮换续期/撤销，本机原生界面确认及 DPAPI 保存。 |
| M16 / dev.23 | 默认程序目录 Data、自身数据拒绝 C 盘、应用和项目迁移、Runtime 临时/缓存目录统一，保留受限执行边界。 |
| M17 / dev.24 | HTTP 关闭与早期错误响应加固；独立原生 TCP 探针确认 AdGuard 回环过滤干扰。 |
| M18 / dev.25 | 主界面按变化更新内容，保留阅读位置和选区；不周期性切换全部按钮或重建稳定列表。无变化/仅显示名和自启选项保存不推进策略版本、不重启工作进程。OAuth 续期保留未过期旧访问令牌；全局导航竞态修正。 |
| M19 / dev.26 | 显式长期授权、原项目凭据绑定、权限变更后持续连接且逐次使用当前权限、scope 续期兼容、v1/v2 存储迁移、失效原因显示。 |

既有核心包含项目隔离、目录与审批、普通/受限/管理员执行后端、原生文件操作、图像、文档及产物传输等；不能把某个项目没有启用的工具误报为产品未实现。候选清单为 37 个工具，实际暴露的工具由当前项目策略决定。

## 4. M19 授权机制和问题结论

用户报告一两轮对话后又要求人工授权，并要求未来无人值守电脑可持续授权至人工取消。随后截图明确显示两条 ChatGPT / Z620 授权和一条客户端登记，第一条连接状态为“项目权限已变更，需要重新授权”。

**已确认原因**：旧限时授权绑定策略版本，用户调整命令身份/权限后版本变化，旧连接失效。两条“已授权”列表项不代表两条当前都有效，也不能单凭截图证明每轮对话都重新授权。没有改配置时的其他反复授权原因，仍需真实客户端续期记录确认。

M19 的具体行为：

- 本机“客户端授权…”默认勾选“长期授权，直到手动撤销”，确认前说明包括项目之后新增的权限；取消勾选保留最长 30 天限时授权。
- 长期授权没有固定到期日，访问令牌仍为 **900 秒**，由客户端自动续期；程序重启、项目目录迁移及权限调整后可继续连接。每次操作检查当前项目权限。
- 长期授权绑定原项目随机凭据摘要，不仅绑定可复用的项目标识。删除后同名重建、更换项目凭据不能继承原连接。禁用项目不能使用连接。
- 刷新仍严格轮换；任何通过校验的旧代数续期凭据重用会撤销整条凭据族。未过期旧访问凭据保留原有效期，最多 8 份历史加当前凭据；超限返回有界 429，不推进刷新代数或撤销现有连接。
- 手动/客户端撤销或移除客户端禁止后续请求，并跨重启保存；不自动停止已经开始的任务。更换 issuer、清除/丢失凭据、账户无法解密及客户端自身删除连接等情况仍需重新连接，不能承诺客户端永不丢失授权。
- scope 按集合检查，接受不同排列和原授权中的访问 scope 子集；轮换刷新凭据保留原授权 scope，禁止扩大范围。
- DPAPI 状态读 v1/v2/v3、写 **v3**。旧连接保留限时规则，不静默升级为长期；回退 dev.25 时须同时恢复升级前数据备份。尚未完成的申请与授权码不跨重启保存。
- 最近令牌端点失败以安全类别、时间及项目保留在本次运行内，界面显示原因；撤销原因加密保存。成功连接不清除最近失败历史。不得输出 token、code、state、PKCE、Cookie 或完整回调查询。

旧 issuer 错误“OAuth authorization response issuer does not match the expected issuer”**未获得完整根因证据**。曾核对公开 issuer 为 `https://shellbot.jsnowly.win`，无末尾斜线；本地回调包含精确 `iss`。用户最终错误页面没有 iss，不能据此认定原始回调缺失。不要通过放宽 issuer/PKCE 校验、取消认证或 URL token 来掩盖问题。

## 5. 最新交付与冻结证据

最新候选（已提供给用户，尚未收到安装 dev.26 和真实客户端验收反馈）：

```text
E:\Program\ShellBot\dist\ShellBot-0.1.0-dev.26-windows-x64-persistent-auth-test-20261021-071250.zip
SHA256 e2ca7934fbb08f972d42e17349ef7a8a8704215cd135a90b91aec0fde8e43ffa
```

文件名时间为 UTC；对应北京时间 2026-10-21 15:12:50。ZIP 为 5,185,041 字节，110 个清单文件，旁有 `.zip.sha256`。默认入口 ShellBot.exe，含 4 个运行组件及文档/许可证/验证记录，不含 Data、私有配置、凭据、开发测试脚本或日常控制脚本。完整验证完成于北京时间 **2026-10-21 15:15:26**。

| 运行组件 | SHA-256 |
| --- | --- |
| Debug WinShellBot.exe | `4f6dc1029533725f05f0d8e5690213f31ab98773f9c27c55b3dab5414d106d29` |
| Release WinShellBot.exe | `9b2c59e12cd1744fc1989c6181bd862c596cafe865af672651c17c93a1395043` |
| Release ShellBot.exe | `066b8bc2112118e355a4daadbd70bcd56c57f225ae76be588c572906c642692f` |

冻结报告目录：`E:\Program\ShellBot\build\persistent-auth-validation`。

- `acceptance.json`：`full_http_regression_verified`，pending 为 0，共 32 份准确程序报告。
- `final-verification.json`：冻结时 179 个源码文件和 75 个本机测试文件集合/摘要核对通过；程序、报告、包与恢复基线核对完成，结束时本轮测试进程为 0，未推送。
- `package-receipt.json`：`persistent_auth_verified_pending_real_client`，含完整候选路径、ZIP 摘要及门禁/解压报告引用。
- `package-gates.json`：29 项拒绝门禁通过；缺失、失败、旧摘要、假 EOF、缺长期授权和控件证据等情况均拒绝打包，拒绝时不改 dist。
- `extracted-runtime.json`：10 组实际解压运行、279 项包检查通过；7 组新解压目录 GUI 流程，3 组网络流程使用下述已排除的固定路径，测试后原组件恢复并核对。
- OAuth Debug / Release 分别 735 / 762 项，原生授权控件 22 项；Release 包含实际等待一次性授权码到期。模拟一年闲置使用独立测试状态时间字段，不是已运行一年。
- 16 组 Release 常规回归通过，完整核心、原生文件/配置、图像、文档、传输及启动流程等按各自原报告范围核对。
- 严格 HTTP EOF：Debug 400 + Release 4000 = **4400 次，无失败**；响应五组共 **5000 次，无失败**，均不逐请求重试。响应故障注入各 28 项，关闭时序各 26 项，独立原生探针 8/8。

保护对象：

- 本轮变更前 37 文件校验备份：`build\baseline-persistent-auth-20261021-144258\manifest.json`。
- dev.25 回滚 ZIP：`dist\ShellBot-0.1.0-dev.25-windows-x64-auth-refresh-test-20261021-055227.zip`；SHA-256 `88d742628791ee1bb4b47aad64b36c7c06327b8006bede6658882f2a79b5515d`。它不能读取 v3，回退须配套旧 Data 备份。
- M18 原始证据和失败包引用：`build\auth-refresh-validation`，包括 `preserved-first-extraction`、`preserved-unexcluded-extraction`、`preserved-before-app-exclusions`、`preserved-initial-navigation`。不要将旧失败删除或重标为通过。
- M19 `preserved-before-scope-subset` 是较早同为 dev.26 的程序/通过记录，先于 scope 子集修正，不能替代最终摘要。其他 `preserved-*` 同样保留。

升级步骤：托盘退出旧版，备份原 Data 与位置设置；把新包内容替换到原固定非 C 盘程序目录，保留 Data、`ShellBot.storage.json` 和 Windows 账户。在客户端 **Z620 这条连接**上重新授权一次，本机核对后勾选长期授权。旧限时连接不自动升级，无需清空全部数据或反复添加同名账户。

## 6. 网络测试环境：必须延续

用户需要代理流量过滤，明确要求 **AdGuard localhost 过滤保持开启**。当前通过条件是 AdGuard 服务运行，加四个固定程序路径排除；不要停服或关闭 localhost 过滤。历史 M17 的短时停服对照已恢复，其 11 秒记录不属于 M18/M19 的操作。

AdGuard 高级设置项为“过滤时按完整路径排除应用”，用户已追加保存，保留原项，多项以分号分隔：

```text
E:\Program\ShellBot\out\x64\Debug\WinShellBot.exe
E:\Program\ShellBot\out\x64\Release\WinShellBot.exe
E:\Program\ShellBot\out\x64\Release\ShellBot-NativeEofProbe.exe
E:\Program\ShellBot\build\tools\ShellBotNetworkTest\ShellBot-TestClient.exe
```

专用测试客户端是固定位置的 Python 运行器，使用现有 E 盘 Python 标准库；不要移动/删除它，不要把系统 Python、浏览器或代理整体加入排除。真实服务名是 `Adguard Service`，M19 最终状态 Running。受限执行环境中的服务查询可能看不到真实服务；需要实际系统状态时使用合适权限只读核对，不能据沙箱空结果判断服务停止。

未排除的解压路径以前仍出现过 10054；固定路径同一程序通过。严格 TCP 半关闭受过滤驱动影响，不可宣称 ShellBot 单独修好了任意过滤环境。M19 实际解压的网络测试把包内四组件逐字节临时放到固定 Release 路径，最后恢复并核对，原始运行路径在报告内。浏览器/本地代理的实际过滤效果未独立验收。

## 7. 真实 MCP 客户端状态与暂缓事项

用户安装了名为 **Z620** 的插件/连接，历史引用 `app://asdk_app_6ad854e20c188191851fa7f0767d5854`。工具命名空间曾为 `mcp__codex_apps__z620`，新对话必须重新发现实际可用工具和模式，不照搬旧工具 schema。

- dev.25 真实只读轮次：48 次调用，46 次成功、1 次参数拒绝、1 次客户端上游传输错误；连续 12/12、四路并发 31/32。错误没有显示明确 EOF/10054；没有跨 15 分钟到期测试，不能算稳定性或续期完全通过。
- 之后保存普通身份配置，出现两次“Authentication for Z620 was requested and accepted. Retry this tool call now.”，一次提示重试仍相同，停止重复授权；用户未保留原始网页跳转错误。
- 多连接 schema 后来要求 `link_id`，按连接名明确选择 Z620 后，`host_info`、`host_capabilities`、`fs_list` 三项正常。不要把缺 link_id 的本地参数错误当作 OAuth 服务器失效，也不要切换到另一个账户寻找更宽权限。
- 最近实测为 **dev.25 / 策略版本 2**，普通身份 `profile=user`；实际文件范围仍为 configured，命令不可用，未暴露 task_start。它不是 dev.26 验收结果，也不是当前远端配置的实时保证。
- `configured_enabled=false` 是后端综合策略判定，不足以认定用户没勾选。既有实现中普通账户命令还要求账户文件范围；受控身份则要求所有者配置精确 `runtime_files`。不要为通过测试自动扩大文件权限，设计是否调整应作为明确新需求。
- **未启动任何真实远端命令，命令验收按用户要求暂缓。** dev.26 真实客户端自动续期、重启恢复、权限修改后保持连接、撤销、未改设置多轮及长期网络运行仍待验。
- 新版本真实管理员 UAC、独立机器/系统及物理托盘等证据也未被本轮自动化替代，不重跑或覆盖用户已经确认的历史人工项。

原始记录位于 `build\real-client-acceptance-20261021`：`z620-actual-20261021T060956Z.json`、`z620-auth-return-062759.json`、`z620-config2-20261021T063133Z.json`。旧 dev.24 记录 `session-mvhmh459.json` 包含失败，不能当作通过；其中旧远端测试 payload 的清理未完成，不能跨项目猜测路径删除。

## 8. 代码入口与开发验证

| 位置 | 用途 |
| --- | --- |
| `core/include/sb.h` | 版本、核心与平台边界；当前 SB_VERSION 为 0.1.0-dev.26。 |
| `WinShellBot/platform/desktop_app.c`、`desktop_store.c`、`desktop_dialog.c` | GUI 应用所有权、项目索引与加密快照、新建/编辑、目录迁移和无变化保存。 |
| `WinShellBot/platform/portable.c` | 程序目录 Data、位置选择/设置、非 C 盘及实际路径检查、应用迁移。 |
| `WinShellBot/platform/manager.c` | 工作进程、项目网关、当前凭据摘要绑定、所有者通道及 OAuth 集成；同项目 HTTP 锁有界等待最多 1 秒。 |
| `WinShellBot/platform/manager_ui.c` | 项目列表/详情增量刷新、阅读位置和选区、排队点击与托盘生命周期。 |
| `WinShellBot/platform/oauth.c`、`oauth.h`、`oauth_ui.c` | 发现、DCR、授权码/PKCE、长期/限时授权、续期/撤销、v3 状态和原生确认界面。 |
| `WinShellBot/platform/project_settings.c`、`core/policy/project_policy.c` | 项目设置、操作策略与命令身份边界。 |
| `WinShellBot/app/http.c`、`http_response.h`、`third_party/civetweb/src/civetweb.c` | HTTP 响应、早期拒绝、有界排空与 EOF 修正。 |
| `tests/oauth_checks.py`、`oauth_ui_checks.py`、`manager_refresh_checks.py` | 真实本机 HTTP+原生授权界面、控件和刷新事件检查。 |
| `scripts/collect-eof-acceptance.ps1`、`package-eof.ps1` | 当前支持 M17/M18/M19 的证据汇总和打包门禁。 |

开发构建仍使用现有 `scripts/build.ps1` 的 Debug/Release 与 `-WithTests`。所有测试 TEMP/TMP 明确放 E 盘；GUI/进程/SCM/实际网络测试可能需要超出受限沙箱的运行权限。原生应用测试只操作本轮独立数据和所属 PID，避免操作用户窗口或真实凭据。

已有 M19 运行入口示例（新版本应新建自己的报告目录和阶段，**不要覆盖 M19**）：

```powershell
Set-Location E:\Program\ShellBot
$env:TEMP='E:\Program\ShellBot\build\persistent-auth-validation\Temp'
$env:TMP=$env:TEMP
& .\scripts\build.ps1 -Configuration Debug -WithTests
& .\scripts\build.ps1 -Configuration Release -WithTests
& .\build\tools\ShellBotNetworkTest\ShellBot-TestClient.exe tests/portable_extra_regression.py m19
& .\build\tools\ShellBotNetworkTest\ShellBot-TestClient.exe tests/desktop_regression.py Release m19
```

独立网络运行脚本为 `build\persistent-auth-validation\network-regression.py`，旧 finalizer 为同目录 `final-verify.ps1`。它们绑定本轮范围和文件，不作为新版本的通用“通过按钮”。测试脚本的版本断言分布在 `tests/smoke.py`、`image_checks.py`、`desktop_app_checks.py`、`oauth_checks.py`；升版本需要一起核对。runner 只按旧程序摘要复用报告不代表修改后的测试已运行，测试或要求变化后须保留旧报告再重新执行。

新开发完成后按实际影响选择回归，最终打包必须绑定准确新程序、源文件及测试集合。不要为文档归档重复全部运行，也不要用早期同版本程序的报告代替最终程序；包验证、真实客户端、真实 UAC 与长期运行分别记录。

## 9. 新对话从这里继续

1. 读取本文，保留当前未提交工作区、最新/回滚包和基线；核对与新目标相关的实际源码。
2. 等用户明确新开发主题。当前没有已经指定的 M20 功能清单，不自动恢复远端命令、触发 OAuth、修改项目权限、清理包或改系统网络设置。
3. 若用户先验收 dev.26，按本节之前的升级与 Z620 连接步骤进行。需要再次出现网页错误时，只收集安全错误文字、分类原因或单独 issuer 值，不索取完整回调与凭据。
4. 原专项细节按需读取：`docs/HANDOFF_20261021_PERSISTENT_AUTH.md`、`HANDOFF_20261021_AUTH_REFRESH.md`、`HANDOFF_20261021_HTTP_EOF.md`；日常用户流程见 `docs/USER_GUIDE.md`。本归档交接不修改这些冻结文件。

本次归档交接的独立摘要核对记录保存为 `build\archive-handoff-20261021\handoff-receipt.json`；它说明新增本文及原冻结文件/候选包保留情况，不替代 M19 原始验收收据。
