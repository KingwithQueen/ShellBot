# Linux / macOS 受限与管理员命令

第八阶段沿用现有 task_start/status/output/input/cancel、审批、请求键、实例绑定与持久审计。Windows 后端和权限语义保持原样。完整示例见 config/posix-restricted.example.json、config/posix-admin.example.json；替换所有示例绝对路径后再验证配置。

受限项目设置 execution.enabled=true、default_identity=restricted，并提供 exact runtime_files 绝对路径数组。初始程序必须在该数组中，且为原生 ELF/Mach-O，拒绝脚本、set-id、符号/硬链接；运行资源不能位于项目暴露目录内。受限命令只访问明确配置的工作目录/授权目录，遵守各目录 read_only；同时允许读取系统动态运行库。不能联网、连接所有者/管理员 socket 或创建子进程。文件工具的策略仍分别执行。

Linux 需要 /usr/bin/bwrap、可用的用户/挂载/PID/网络命名空间和 seccomp。新文件系统中只挂载指定目录、只读运行资源、系统运行库与独立的 64MiB 临时目录；seccomp 禁止创建进程、socket、跨进程访问、命名空间及挂载等操作。没有暴露宿主 /proc、home 或 /run。Mac 使用系统 Seatbelt 的 deny-default 策略；只读系统库、精确运行资源和配置目录之外的文件默认拒绝，网络和进程创建默认拒绝。该 Apple 接口已弃用，未来系统若不再支持，必须拒绝运行，不能回退普通命令。

每个受限后端启动时，在同一隔离配置内实际运行随包的 ShellBot-RestrictedProbe，验证私有数据不可读、网络和 fork 被拒绝；失败时拒绝项目启动。实际命令在执行前仍须成功建立隔离。通过 host_capabilities.execution_status.restricted_verified 查询结果；配置摘要不等于验证成功。隔离不是虚拟机，不防御内核漏洞；用户声明的程序及运行库依然应可信。Linux 初始程序后自行替换进程映像仍保留同一沙箱，不能创建第二个任务进程。

管理员命令使用显式的 identity:administrator；普通网关和项目工作进程始终非 root。项目还必须独立设置 administrator_allowed=true、default_identity=current_user、files.scope=current_user，并取消命令涉及的只读限制。只授予命令任务及其审批查询/取消，不提供管理员文件、媒体或桌面代理。普通命令不因管理员代理存在而提升。

本地所有者先把完整程序包和注册表副本放到 root 拥有且各级目录不可被普通用户改写的位置；注册表必须与普通项目的配置一致。准备独立 root 私有状态目录。不要更改现有普通 Data 的所有者。然后由所有者在目标机器显式运行：

```sh
sudo /absolute/root-owned/bin/ShellBot --admin-agent \
  --projects /absolute/root-owned/projects.json --project example \
  --owner-uid 1000 --admin-dir /absolute/root-owned/admin-state
```

Mac 的实际 uid 可用 id -u 查询，常见为 501；Linux 示例值不能照抄。sudo 自行向所有者索取密码；ShellBot 不读取、缓存密码，不设置免密 sudo，不自动弹窗提权。管理员程序、注册表、既有 admin-state 及其所有祖先必须 root 拥有、无符号链接且组/其他人不可写；状态目录须为 0700，注册表和程序不能有硬链接或 set-id 位。

代理等待最多 120 秒，随后启动/重启对应的普通项目工作进程。连接通过固定 /run（Mac /private/var/run）下的独占 socket；两端校验实际 UID；代理还校验连接进程确实运行同一份 root 拥有的可执行文件（普通 worker 也须从该安装位置启动）。代理核对完整项目命令策略、路径、数据目录、审批方式、资源限制和实例。只授权该配置、该项目、该普通 worker 的一次连接期，不接受远程请求重新注册、切换项目或扩权。没有匹配代理时普通项目仍可运行，但管理员任务明确失败。

管理员审批保存在独立的 root 状态；普通审批不生效。返回 pending 后，所有者用 sudo ShellBot --inspect/--approve ID --data-dir ADMIN_STATE 检查并批准，再原样重试包含 approval_id 的请求。审批绑定实际程序、cwd 元数据与实例；stdin 写入同样审批。查看结果、取消和历史按原调用者归属。管理员任务具有 root 账户权限，不受文件根隔离，也可能修改系统或代理自身，只有可信所有者才应授予。

Ctrl+C 退出代理、sudo ShellBot --stop --data-dir ADMIN_STATE，或暂停/停止普通项目都会撤销本次连接并取消所属任务；连接关闭不自动重连、重试、提权或降级。后续需所有者显式启动新代理和新项目实例。历史状态保留，不重放旧任务。不要将代理配置为无人确认的永久免密提权服务。

自动验证与真实 sudo/root、系统隐私许可和外部 MCP 人工验收分别记录；本阶段不修改 sudoers、不重启或注销系统。

原生接口依据：[Bubblewrap](https://github.com/containers/bubblewrap/blob/main/bwrap.xml)、[Linux seccomp](https://docs.kernel.org/userspace-api/seccomp_filter.html)。Mac 隔离接口的弃用状态按目标机器的 SDK sandbox.h 与 sandbox_init(3) 文档核对。

若代理遭 SIGKILL 或断电，root 管理员须先确认没有存活代理，再清理其固定 socket 残留；程序不会自动覆盖已有端点。
