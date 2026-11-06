# 固定版本的原生依赖

POSIX 第三阶段：Linux 的凭据后端动态使用系统 OpenSSL 3（libcrypto）及 libsecret/GLib/GIO，CMake 要求对应开发包；这些系统库不随开发包捆绑。macOS 使用系统 Security、CoreFoundation 与 CommonCrypto，不依赖额外下载的密码库。Windows 的固定依赖、构建和发布方式不变，说明见 [POSIX 凭据与 OAuth](../docs/POSIX_OAUTH.md)。

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

## POSIX media dependencies

Linux/macOS x86_64 use the separate PDFium 157.0.8086.0 no-V8/no-XFA artifacts under `pdfium/linux-x64` and `pdfium/mac-x64`. Each folder retains its publisher provenance (bblanchon, independently built from upstream PDFium), archive SHA-256, library SHA-256, build arguments and complete license set. The shared public headers match both publisher archives. CMake and the runtime verify fixed library hashes; Windows artifacts are unchanged.

`stb/stb_image.h` is pinned to nothings/stb commit `a7d4c2175630e1ea5b6f8dd00ab96fed0b97b96d`, with exact hashes in `stb/PROVENANCE.json` and the upstream dual MIT/public-domain license. POSIX builds enable only memory PNG/JPEG decoding. System Expat (MIT), zlib, and Linux Pango/Cairo or Mac CoreText provide bounded XML parsing and text rasterization. These system dependencies are not redistributed here. See [media tools](../docs/POSIX_MEDIA.md).

## Windows PCRE2 模式搜索（1.1.3）

PCRE2 固定为官方 10.49，源码发布摘要和构建选择保存在 [provenance.json](pcre2/provenance.json)。下载源为 PCRE2Project 官方 GitHub release，摘要与发布资产 SHA-256 匹配。保留实际使用的 C 源、头文件、上游构建说明、[LICENCE.md](pcre2/LICENCE.md)、COPYING 与 AUTHORS.md；BSD-3-Clause WITH PCRE2-exception 等完整声明随正式包分发。

Windows MSBuild 单独构建静态 8 位 Unicode 库，无 JIT、无额外运行 DLL。config.h 由上游 generic 配置启用 SUPPORT_PCRE2_8/SUPPORT_UNICODE，pcre2.h 和默认字符表由上游模板复制；pcre2.vcxproj 为本项目构建入口。查询使用 UTF-8/UCP 和有界匹配/编译，不运行 shell 或外部搜索器。Mac/Linux 暂不接入此依赖。ConPTY 与 UIA 使用 Windows 系统 API，不分发新的系统组件。

## Windows D3D11 配套采集（1.1.7）

Microsoft Detours 固定为官方 v4.0.1，提交及未修改源码的逐文件摘要见 [provenance.json](detours/provenance.json)。仅保留实际构建所需文件及原始 MIT 许可，Windows x64 静态链接到 ShellBotGraphics 配套程序；发布包保留许可证和来源声明。采集代码由本项目实现，没有复制 OBS 的 GPL 代码。配套程序只在显式启动新应用时加载自有采集模块，支持范围见 [图形采集说明](../docs/GRAPHICS_CAPTURE.md)。
