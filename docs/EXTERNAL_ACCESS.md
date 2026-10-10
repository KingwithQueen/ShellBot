# 本机 HTTP 与已有隧道

更新日期：2026 年 10 月 6 日。版本：`0.1.0-dev.5`。

按照用户最新确认的范围，ShellBot 只提供本机端口，默认 HTTP 监听 127.0.0.1:8765。域名、DNS、HTTPS 和隧道由已有 cloudflared 或其他外部环境负责，不开发或管理这些组件，不要求用户执行域名检查。现有 bearer 与本机审批继续使用。

未配置 public_url 时，程序接受已有本机代理转发的合法 Host 和 http(s) Origin，请求仍必须携带正确 bearer；不使用 Cookie 认证，也不开放 CORS。它不会从请求头授予审批权限。只需端口的部署不需要填写域名，public_url 作为可选的域名限制和地址报告功能保留。

## 可选：填写地址

在配置文件的 http 内设置 public_url：

```json
{
  "roots": [
    { "id": "workspace", "path": "C:/ShellBotWorkspace", "read_only": true }
  ],
  "http": {
    "port": 8765,
    "token_env": "SHELLBOT_TOKEN",
    "public_url": "bot.example.com"
  }
}
```

也可使用命令行：

```powershell
$env:SHELLBOT_TOKEN = & .\WinShellBot.exe --generate-token
.\WinShellBot.exe --http --config .\shellbot.example.json --public-url bot.example.com
```

目录必须预先存在。单独填写域名时默认 https，完整地址 https://bot.example.com/ 会规范为 https://bot.example.com。允许明确的 http scheme 和端口，例如 https://bot.example.com:8443；默认端口会移除，域名不区分大小写。当前接受 ASCII 域名，国际化域名使用其 punycode 表示。不接受 URL 凭据、查询参数、片段、通配符、IPv6 字面量或路径前缀。

MCP 客户端使用 https://bot.example.com/mcp。host_capabilities 的 public_endpoint 返回该地址；没有填写地址时不返回这个字段，public_url:"" 由外部代理决定域名，仍验证 Host/Origin 的格式和 bearer。填写地址后则启用该 origin 与回环地址的白名单限制。命令行地址覆盖配置文件地址。

## 已有环境的连接约定

ShellBot 仍只监听 127.0.0.1:配置端口。已有隧道将外部 /mcp 转发至该监听地址。未配置 public_url 时，代理可保留外部域名和端口，存在 Origin 时验证格式；配置 public_url 后则只接受明确指定的外部地址及回环地址。格式不合法、重复的安全头、URL 凭据和错误 bearer 均拒绝。

SHELLBOT_TOKEN 是现有接口的 bearer，客户端请求需携带 Authorization: Bearer <令牌>。填写域名不生成 OAuth 发现、登录网页或长期连接授权。外部环境自己的认证如何对接，由该环境负责。

程序不会根据 Forwarded、X-Forwarded-Host 或 X-Forwarded-Proto 自动扩大允许的地址，也不根据请求 Host 生成配置。管理和批准仍通过本机命名管道完成。

程序不自动探测公网地址，也不启动、停止或安装隧道组件。当前开发验证本机接口和模拟代理转发；用户已经在其他程序上验证外部隧道，ShellBot 不要求再次检查域名。实际 MCP 客户端功能与本机协议自检分别记录。已知的偶发 HTTP 关闭连接重置继续保留诊断。

## 可选：独立端点检查（当前不需要执行）

以下流程保留给将来明确要求端点检查的场景，不是当前开发或交付的前置条件；该独立脚本要求主机已填写被测 public_url，默认只监听本地端口的部署不需要它。

ShellBot 的本机源站是 **http://127.0.0.1:8765**，不是 https://localhost:8765。外网入口可以继续使用 HTTPS；隧道的 Service URL 必须与本机服务的 HTTP 协议匹配。使用明确 IPv4 地址也与主机的监听地址一致。Cloudflare 的 [HTTP 协议说明](https://developers.cloudflare.com/cloudflare-one/networks/connectors/cloudflare-tunnel/routing-to-tunnel/protocols/) 描述了外部 HTTPS 转发到本机 HTTP 的方式。

Test-ShellBot.ps1 启动隔离测试实例，结束时会关闭这些实例，不会留下运行在 8765 的长期服务。域名检查需要另行启动主机。下面是只开放主机查询的诊断实例，没有授权任何文件目录，不加载或覆盖原有配置。

在 dev.4 解压目录准备 Test-ShellBot-Endpoint.ps1 后，同一个 PowerShell 窗口运行：

```powershell
$env:SHELLBOT_TOKEN = & .\WinShellBot.exe --generate-token
$shellbotProcess = Start-Process -FilePath (Join-Path $PWD 'WinShellBot.exe') -ArgumentList @('--http', '--public-url', 'https://shellbot.jsnowly.win', '--data-dir', 'C:\ShellBotTunnelTestData') -WindowStyle Hidden -PassThru
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Test-ShellBot-Endpoint.ps1 -PublicUrl https://shellbot.jsnowly.win
```

实际域名替换示例地址。ShellBot 默认端口 8765；已有主机占用该端口时应使用它的实际令牌和配置，不另行启动第二个实例。主机通过 --public-url 或配置 http.public_url 接收真实外部 origin，而不是源站 http://127.0.0.1:8765。

该脚本只调用拒绝检查、/healthz、MCP initialize、notifications/initialized、tools/list、host_info 和 host_capabilities，验证配置中的 public_endpoint 与访问地址一致。不读取文件、提交审批或停止主机，也不管理隧道。脚本关闭重定向且不跳过 HTTPS 证书验证；响应缓冲上限 256 KiB，每次请求超时默认 15 秒。不记录访问令牌、HTTP 正文或任意异常消息，报告记录状态码、阶段及耗时。

令牌从当前进程的 SHELLBOT_TOKEN 环境变量读取，或通过 -TokenEnv 指定变量名。未设置令牌时，只检查匿名拒绝并返回 needs_token，不宣称完整端点通过。只有拒绝检查、初始化、工具发现和两次只读工具调用都通过才返回 passed；脚本通过仍不代表真实模型客户端验收。

报告保存在脚本旁的 endpoint-test-results/<时间和编号>/report.json，也可用 -OutputDirectory 指定目录。把 report.json 回传即可，不要回传令牌。检查完成后可用原诊断实例的数据目录停止它：

```powershell
.\WinShellBot.exe --stop --data-dir C:\ShellBotTunnelTestData
```
