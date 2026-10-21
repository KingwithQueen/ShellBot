# ShellBot M17 / dev.24：HTTP EOF 修复与待验边界

用户要求先修复 HTTP EOF 连接重置，回归通过后再打包，真实 MCP 客户端由用户验收。便携数据只使用非 C 盘的要求保持；已授权 C 盘文件操作保持。沿用 main / 5acbc24437d109c9f0815baa75ca5c41fd7e9c0e，所有改动未提交、未推送。

开始前 `build/baseline-eof-20261021-113112` 复制并逐项 SHA-256 校验 269 个源码、文档、本地测试、Debug/Release 组件及 M16 收据。旧 ZIP、原始失败和基线保留。运行数据、日志与构建临时文件位于 E 盘 `build/eof-validation`；未操作真实用户项目。

dev.23 原程序的 80 次独立 EOF 请求中，oversize-no-body 5/20 在完整响应后的 EOF 阶段报 10054，oversize-half-close 20/20 没有响应；另外两组各 20 次通过。原报告 `dev23-before.json` 保留。临时诊断记录关闭阶段 `recv` 返回 10060；诊断只记录端口、计数及错误码，不记录 URL、请求正文或凭据。诊断源码与程序分别保存在该目录，最终产品不含这些打印。

产品修正：发送响应后先 `shutdown(SD_SEND)`，再以非阻塞接收及 50 ms 等待进行最多 1 秒的尾部排空；不以会改变连接状态的 `SO_RCVTIMEO` 制造接收超时。不在发送错误响应前等待客户端可能永远不发的正文；正常复用请求保持原有正文处理。接受队列从 4 增到 32，工作线程仍为 4，保持资源有界。认证、OAuth、文件范围、审批、管理员权限和 37 个工具保持。Debug/Release dev.24 构建已完成，完整 HTTP 边界一次 221 项通过；这不能代替重复 EOF 及全部回归。

独立对照还发现，完全不含 ShellBot/CivetWeb/HTTP 的纯 C Winsock 探针在 127.0.0.1 与 127.0.0.2 上均出现发送半关闭后提前收到 EOF、丢失对端回复；Python 独立服务亦复现，并出现对端收到重复请求字节。保留 `native-probe*.log`、`loopback-diagnostic.json` 等原始结果；这些结果提示环境干扰，不能单凭驱动存在就判定某组件为根因。

本机运行 AdGuard，另加载 IDM、NetFilter 与其他网络驱动。用户明确授权临时暂停 AdGuard 过滤做对照并立即恢复。第一次普通所有者调用不能停止服务，原状态仍 Running，收据 `adguard-control-without-elevation.json`。随后 UAC 启动返回“操作已被用户取消”，没有执行暂停；已向用户确认后续方式。在收到新的确认前，不重复启动 UAC，不停止其他服务或调整任何过滤配置。

相关一手资料：

- [Microsoft：SO_RCVTIMEO 与连接状态](https://learn.microsoft.com/en-us/windows/win32/winsock/sol-socket-socket-options)
- [Microsoft：正确关闭与 linger](https://learn.microsoft.com/en-us/windows/win32/winsock/graceful-shutdown-linger-options-and-socket-closure-2)
- [AdGuard 官方项目同类回环半关闭报告](https://github.com/AdguardTeam/AdguardForWindows/issues/6233)：用户报告不等于本机已完成因果验证。

新包门槛：完整原有回归、便携、OAuth、界面与阅读位置、包含 EOF 的完整边界、Debug/Release 重复 EOF、无重试的响应压测和 ZIP 解压后的真实运行均须有最终准确程序摘要绑定的通过报告。不得使用 responses-only 替代 EOF，不得将连接重置视作 EOF，不得重试失败请求来抹掉失败。当前阶段尚未生成 dev.24 分发包。

## 对照与当前结果

用户再次确认后，固定脚本经 UAC 执行，AdGuard 停服 11.282 秒并在 finally 中恢复 Running。停服期间独立原生探针 8/8、旧 dev.23 和新 dev.24 各 16 次严格 EOF 全部通过；恢复服务后同一原生探针再次 4/8 失败。这个开—关—开对照确认本机 AdGuard 过滤造成了半关闭异常，不能把旧版偶发 EOF 问题全部归因于 ShellBot。用户随后手动关闭 localhost 过滤并明确回复“已经关闭了”；服务保持 Running，同一原生探针恢复 8/8 通过。

用户询问该选项是否影响走本地代理的流量。已明确纠正“保留外网过滤”的笼统说法：浏览器到本机代理的连接属于回环流量，关闭后能否在代理出站继续过滤取决于转发方式，不能保证。长期可恢复所需的回环过滤并按应用定向排除 MCP 相关进程，但这种配置未在本轮验证；没有自动改变 AdGuard 配置，没有暂停其他过滤服务，没有调整 DNS、系统代理或隧道。

dev.24 的独立加固也有行为证据：同条件下，旧 dev.23 对未发送的超限正文等待并超时，`dev23-early-reject-before.json` 原失败保留；新 Debug/Release 各 26 项及时拒绝、迟到正文排空和活动连接下停止检查通过，短写及故障注入各 28 项通过。严格 EOF Debug 400、Release 4000 次全部通过，无失败请求重试。原 16 组 Release 完整回归通过，核心本轮 2507 项、M4 642 项（轮询数量可能变化）、原生文件 43、真实配置表单 74、完整 HTTP 边界 221。便携 Debug/Release 各 60、OAuth Debug 269/Release 281（含真实过期）、授权可见控件 19、阅读位置各 20、桌面流程 64、公开入口 11 和迁移退出 9 均通过。准确产物与计数以 build/eof-validation 收据为准。

当前冻结主程序 SHA-256：Debug `6f38eb1839e7760e2228cc958207f270e0f801ed06ccd2321188001e78c920db`；Release `6627dc1283ce32e917b285e9c86b67f746ede83ceb2b2f551c8179d172a821c0`。维持 C17、静态 CRT、37 个 MCP 工具及原审批边界。M17 独立收集器与打包器要求完整边界、Debug/Release EOF、关闭时序与故障注入，以及所有既有功能报告；拒绝 responses-only 替代、短缺采样、失败或过期程序摘要。新包地址、ZIP SHA-256、负向门槛、解压运行与最终摘要分别记在 package-receipt.json、package-gates.json、extracted-runtime.json、final-verification.json。只有实际存在且通过的报告算完成，不能凭生成脚本宣称打包或解压完成。