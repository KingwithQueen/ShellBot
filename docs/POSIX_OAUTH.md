# Linux / macOS 凭据存储与 OAuth

适用于 POSIX 后台开发版第三阶段。Windows 的 DPAPI、授权窗口和 OAuth 继续使用现有实现；两种存储格式不互读。POSIX OAuth 由管理器提供，本机所有者通过终端确认，尚无原生管理窗口。

## 保存项目凭据

默认将随机主密钥交给 macOS Keychain 或 Linux Secret Service；项目 bearer 和 OAuth 状态以认证加密文件保存到管理器的私有 Data。Mac 使用系统 Security/CommonCrypto，Linux 使用 OpenSSL 3 和 libsecret。密钥环须已解锁且当前账户可访问。Mac 禁止交互，Linux 拒绝服务的提示请求，不会自动打开解锁窗口或降级存储。

管理器 Data 与项目 Data 分开，父目录须已存在，Data 属于当前账户且权限为 `0700`，不能与配置的工作目录重叠。替换以下示例中的绝对路径：

```sh
ShellBot --credential-generate example --token-env SHELLBOT_EXAMPLE_TOKEN --manager-data-dir /absolute/private/ManagerData
ShellBot --credential-status --manager-data-dir /absolute/private/ManagerData
```

生成命令只返回保存状态。导入已有 bearer 使用 `--credential-set example --token-env NAME --manager-data-dir DIR`，从标准输入读取一行 32～128 位十六进制字符，不能通过命令行参数传秘密。`--credential-status` 只列项目和引用；`--credential-remove ID` 删除保存的条目。确需手动配置 bearer 时，`--credential-export ID --output NEW_FILE` 显式导出到新建的 `0600` 明文文件，拒绝覆盖。上述命令均须在管理器停止后执行。

管理器通过 `--credential-store` 使用保存的项目凭据；`--oauth` 自动启用存储。无保存条目的项目仍可使用原 `token_env`；保存条目与环境值或环境引用冲突时拒绝启动，避免静默改变身份。不会自动把环境变量写入磁盘。启用项目间 bearer 必须不同。

## 纯 SSH 环境

没有可用的已解锁密钥环时，可**明确选择**私有密钥文件模式。程序不自动选择此模式。密钥目录须为独立的 `0700` 目录，不能与管理器 Data、配置的工作目录互相包含。

```sh
mkdir -m 700 /absolute/private/ShellBotKeys
ShellBot --credential-key-create /absolute/private/ShellBotKeys/master.key
ShellBot --credential-generate example --token-env SHELLBOT_EXAMPLE_TOKEN --manager-data-dir /absolute/private/ManagerData --credential-key-file /absolute/private/ShellBotKeys/master.key
```

后续离线凭据命令和管理器均附加同一个 `--credential-key-file`。密钥文件须为当前账户所有的 `0600` 单链接普通文件，拒绝符号链接；现有密钥永不自动覆盖，丢失后重新生成不能解密旧数据。此模式依赖账户和文件权限保护主密钥，**不等同于系统钥匙串保护**。分别保管密钥与 Data，不放入工作区、构建包、日志或版本库。

Mac 可用 `--credential-keychain /absolute/private/custom.keychain` 选择已有、已解锁的私有钥匙串，不修改默认钥匙串或搜索列表。未签名开发程序改路径或升级后可能被系统 ACL 拒绝；须所有者本机处理，程序不会放宽 ACL 或替换旧密钥。SSH 测试使用独立测试钥匙串，不读取已有用户凭据。

## 开启 OAuth 与本机确认

复制 `config/posix-oauth.example.json`，设置真实目录、Data 和精确 HTTPS `public_url`：

```sh
ShellBot --manager --http --oauth --projects /absolute/private/projects.json --manager-data-dir /absolute/private/ManagerData --start-project example
ShellBot --oauth-status --manager-data-dir /absolute/private/ManagerData
ShellBot --oauth-approve 64位申请编号 --persistent --manager-data-dir /absolute/private/ManagerData
ShellBot --oauth-deny 64位申请编号 --manager-data-dir /absolute/private/ManagerData
ShellBot --oauth-revoke 64位连接编号 --manager-data-dir /absolute/private/ManagerData
ShellBot --oauth-remove-client 64位客户端编号 --manager-data-dir /absolute/private/ManagerData
```

私有文件密钥模式在启动命令中附加同一个密钥选项。先核对申请编号、项目、客户端名称、回调和 scope 再批准。`--persistent` 明确授予首次确认后持续到手动撤销的连接，包括项目之后允许的操作；文件范围、操作审批、命令开关和管理员边界仍独立执行。省略该选项为限时授权，不自动升级。申请五分钟有效，决定后的授权码最多一分钟；访问令牌 900 秒，限时连接最长 30 天。

客户端 MCP 地址为 `https://你的域名/projects/example/mcp`。ShellBot 仍只监听 `127.0.0.1`，TLS、证书、域名和隧道由用户配置。单独的固定项目 HTTP 入口继续使用环境凭据，不充当授权服务器；未传 `--oauth` 的既有模式继续工作。

| 端点 | 用途 |
| --- | --- |
| `/.well-known/oauth-authorization-server` | 授权服务器元数据 |
| `/.well-known/oauth-protected-resource/projects/ID/mcp` | 精确项目资源元数据 |
| `/oauth/register` | 有容量与频率限制的公共客户端注册 |
| `/oauth/authorize`、`/oauth/flow/ID` | S256 PKCE、浏览器 cookie 绑定的待审批申请 |
| `/oauth/token` | 单次授权码兑换、严格轮换刷新 |
| `/oauth/revoke` | 客户端撤销自身连接 |

401 响应带项目资源元数据挑战。注册不产生访问授权，只接受公共客户端 `none`、授权码和 S256，拒绝隐式/密码授权、客户端秘密认证、任意回调、不同项目资源及 scope 扩张。当前 scope 为 `mcp` 与可选 `offline_access`。页面只等待本机决定；没有浏览器批准自身访问的入口，也没有远程 owner HTTP 接口。所有响应禁止缓存。

刷新令牌每次轮换，重用旧令牌撤销该连接。scope 可重排或缩小，不能超过原授权集；缩小本次访问 scope 不改变原刷新授权范围。刷新不延长旧访问令牌；同时存活租约达到上限时返回限流，应继续使用尚有效的访问令牌，避免高频刷新。

长期连接绑定项目 ID、原 bearer 和项目 Data 的物理身份。保留身份的策略更新允许继续连接，仍逐次执行当前权限；轮换 bearer 或同名项目使用新 Data 时旧授权不能继承。新项目应创建新 Data 并生成新凭据；显式重用原 Data 和原凭据属于恢复原项目。暂停/停止仍阻止路由。

## 加密与故障恢复

文件封装使用随机 IV 的 AES-256-CBC 和独立随机子密钥的 HMAC-SHA-256。MAC 覆盖版本、IV、长度、账户/目录/文件用途上下文和密文；先验证再解密。临时密文先同步落盘，保存上一份密文，再原子重命名并同步目录。正常仅保留 `credentials.sealed.previous`、`oauth.sealed.previous` 各一份。

损坏、错误密钥、宽松权限、当前文件缺失但恢复文件仍在时拒绝使用，不创建空存储、不自动回滚。运行中保存失败时不返回新令牌，OAuth 认证和变更停止直至恢复重启；已配置的静态 bearer 仍是独立入口。保留 `.previous`、`.pending-*` 和诊断供人工恢复，恢复旧刷新状态可能回退防重放记录。

上下文绑定管理器 Data 的规范路径和 UID，直接搬移、换账户或复制机器不属于已支持的迁移。本地审批、任务、审计继续使用私有权限文件，未改为全 Data 加密。加密不隔离恶意同 UID、已授权的普通账户命令或整盘回滚。

验证包括实际 Keychain/Secret Service 隔离条目、重启、破坏与保存失败、授权/撤销、并发刷新重用和过期。过期只改变测试进程内时钟，不改系统时间，测试辅助库不链接/安装进产品。脚本 HTTP 测试不等于真实 HTTPS 隧道、浏览器或 MCP 客户端已验收；Apple Silicon、其他发行版/旧 libc、签名升级及长期运行另行验证。

协议依据：[RFC 9700](https://www.rfc-editor.org/rfc/rfc9700.html)、[RFC 9728](https://www.rfc-editor.org/rfc/rfc9728.html)。
