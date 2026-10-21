# 固定版本的原生依赖

2026-10-20 / dev.20 本地补丁：CivetWeb 的 civetweb.c / civetweb.h 新增 mg_disable_keep_alive，仅将当前连接标记为响应完成后关闭。下方归档摘要记录原始上游发行包，不能用来宣称当前 CivetWeb 源码逐字节未修改；本地修改随源码清单校验，MIT 许可保持。未更改 PDFium、yyjson 的依赖字节或版本。

ShellBot 自有代码使用 C17。yyjson / CivetWeb 源文件直接编译进程序；PDFium 使用固定版本的旁置原生 DLL，默认构建复制对应架构文件。产品不需要 Node.js、Python、.NET、Microsoft Office 或运行时下载依赖。

| 组件 | 固定版本 | 原始归档 SHA-256 | 许可 |
| --- | --- | --- | --- |
| [yyjson](https://github.com/ibireme/yyjson/releases/tag/0.13.0) | 0.13.0 | `fe8e0136947cf0066794a602ffc25cb50fce0ec756b1458a8225ab12ae557f6b` | [MIT](yyjson/LICENSE) |
| [CivetWeb](https://github.com/civetweb/civetweb/releases/tag/v1.16) | 1.16 | `9f98e60ef418562ae57d6c8e64fb1b2d2b726201b7baee23b043d15538c81dac` | [MIT](civetweb/LICENSE.md) |
| [PDFium binaries](https://github.com/bblanchon/pdfium-binaries/releases/tag/chromium/8086) x64 | 157.0.8086.0，无 V8 / XFA | `1fd8af952832dbb0eb16d9249f68fe09e5f5ebf7c3dd9f6066ea2720cc28487d` | [发行工具 MIT](pdfium/LICENSE)及 [上游许可](pdfium/licenses/pdfium.txt) |
| PDFium binaries Win32 | 同上 | `4958b594e9709df44b8d69af3f90e62be54e335785c469217a2d364e6f141a54` | 同上 |

来源：分别下载 `https://codeload.github.com/ibireme/yyjson/zip/refs/tags/0.13.0` 和 `https://codeload.github.com/civetweb/civetweb/zip/refs/tags/v1.16`。保留 yyjson 的 src/yyjson.c、src/yyjson.h、LICENSE，以及 CivetWeb 的 src/civetweb.c、src/*.inl、include/civetweb.h 和 LICENSE.md；yyjson 源文件未修改；CivetWeb 本地补丁见上方说明。

CivetWeb 编译关闭 TLS、CGI、WebSocket、自动文件映射及文件系统访问。ShellBot 在路由回调中处理所有请求，文件工具使用独立的平台权限接口。当前 HTTP 只监听回环地址；外部 HTTPS 由用户已有的反向代理或隧道提供；OAuth 连接认证已在原生管理界面集成。

Release 使用静态 MSVC 运行库。1.0.0 沿用此处固定依赖，并在发布包内保留完整许可和来源记录。

PDFium DLL 来自 bblanchon/pdfium-binaries 的固定 chromium/8086 发布；发布者独立于上游 Google/Foxit，本仓库未自行构建 PDFium。归档 SHA-256 与发布元数据一致后，保留 DLL、两个公开头文件、VERSION、x64 构建参数及完整 17 项上游/依赖许可；来源、字节数及两架构摘要见 [provenance.json](pdfium/provenance.json)。未声称完成发布签名/构建证明核验。上游核心为 BSD，包含的 FreeType、HarfBuzz、ICU、LCMS、JPEG 等各有许可，不能只分发外层 MIT。

运行时仅从主程序的绝对同目录加载 DLL，先验证实际文件及固定 SHA-256，并持有只读句柄防止替换；不存在、不符或为重解析/多链接文件时拒绝 PDF 功能。x64 DLL 摘要为 `1f2aee4286b8cf0413ab4562b7121aa454f9cfd9afd6ca8609bcef80927b3f80`，Win32 为 `095f17644e3dc324ac7800d5f1bbdc38bcfabd1f52045617d8050901ae9544b3`。当前 x64 已构建和本机验证，Win32 仅保留依赖，未纳入本轮产品验收。

Office 使用 Windows 系统 OPC / XmlLite 和既有 WIC / GDI，未增加 Office 自动化或外部格式转换程序。PDFium 没有创建 JavaScript 或交互表单环境；文档解析仍在现有文件后端进程内，资源预算不等于独立解码进程隔离，边界见 [文档阅读](../docs/DOCUMENT_READING.md)。

M13 收尾补丁：CivetWeb Windows 关闭连接前先有界接收对端结束，随后发送 FIN，避免已写响应在过早关闭时被重置；保留固定版本与许可，临时诊断及无效的 TCP 参数实验未进入最终源码。当前 TCP 对照、连接预算和限制见 docs/HTTP_DIAGNOSTICS.md。
