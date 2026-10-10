# 固定版本的原生依赖

ShellBot 自有代码使用 C17。以下源文件直接编译进程序，不需要 Node.js、Python 或动态下载运行依赖。

| 组件 | 固定版本 | 原始归档 SHA-256 | 许可 |
| --- | --- | --- | --- |
| [yyjson](https://github.com/ibireme/yyjson/releases/tag/0.13.0) | 0.13.0 | `fe8e0136947cf0066794a602ffc25cb50fce0ec756b1458a8225ab12ae557f6b` | [MIT](yyjson/LICENSE) |
| [CivetWeb](https://github.com/civetweb/civetweb/releases/tag/v1.16) | 1.16 | `9f98e60ef418562ae57d6c8e64fb1b2d2b726201b7baee23b043d15538c81dac` | [MIT](civetweb/LICENSE.md) |

来源：分别下载 `https://codeload.github.com/ibireme/yyjson/zip/refs/tags/0.13.0` 和 `https://codeload.github.com/civetweb/civetweb/zip/refs/tags/v1.16`。保留 yyjson 的 src/yyjson.c、src/yyjson.h、LICENSE，以及 CivetWeb 的 src/civetweb.c、src/*.inl、include/civetweb.h 和 LICENSE.md；源文件未修改。

CivetWeb 编译关闭 TLS、CGI、WebSocket、自动文件映射及文件系统访问。ShellBot 在路由回调中处理所有请求，文件工具使用独立的平台权限接口。当前 HTTP 只监听回环地址；远程 HTTPS 及 OAuth 接入仍在后续计划内。

Release 使用静态 MSVC 运行库。首次发行前需要复核这些固定版本的适用性、漏洞修复与组件许可，并保留对应发布记录。
