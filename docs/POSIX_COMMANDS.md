# Linux / macOS 受限与管理员命令

第八阶段沿用现有 task_start/status/output/input/cancel、审批、请求键、实例绑定与持久审计。Windows 后端和权限语义保持原样。完整示例见 config/posix-restricted.example.json、config/posix-admin.example.json；替换所有示例绝对路径后再验证配置。

受限项目设置 execution.enabled=true、default_identity=restricted，并提供 exact runtime_files 绝对路径数组。初始程序必须在该数组中，且为原生 ELF/Mach-O，拒绝脚本、set-id、符号/硬链接；运行资源不能位于项目暴露目录内。受限命令只访问明确配置的工作目录/授权目录，遵守各目录 read_only；同时允许读取系统动态运行库。不能联网、连接所有者/管理员 socket 或创建子进程。文件工具的策略仍分别执行。

Linux 需要 /usr/bin/bwrap、可用的用户/挂载/PID/网络命名空间和 seccomp。新文件系统中只挂载指定目录、只读运行资源、系统运行库与独立的 64MiB 临时目录；seccomp 禁止创建进程、socket、跨进程访问、命名空间及挂载等操作。没有暴露宿主 /proc、home 或 /run。Mac 使用系统 Seatbelt 的 deny-default 策略；只读系统库、精确运行资源和配置目录之外的文件默认拒绝，网络和进程创建默认拒绝。该 Apple 接口已弃用，未来系统若不再支持，必须拒绝运行，不能回退普通命令。

每个受限后端启动时，在同一隔离配置内实际运行随包的 ShellBot-RestrictedProbe，验证私有数据不可读、网络和 fork 被拒绝；失败时拒绝项目启动。实际命令在执行前仍须成功建立隔离。通过 host_capabilities.execution_status.restricted_verified 查询结果；配置摘要不等于验证成功。隔离不是虚拟机，不防御内核漏洞；用户声明的程序及运行库依然应可信。Linux 初始程序后自行替换进程映像仍保留同一沙箱，不能创建第二个任务进程。

管理员操作使用显式的 identity:administrator；普通网关和项目工作进程始终非 root。项目还必须独立设置 administrator_allowed=true、default_identity=current_user、files.scope=current_user。第十阶段补齐文件读写/管理、图片和文档读取/预览、产物上传下载及相应审批。命令仍须另外启用 execution.enabled 并取消命令涉及的只读限制；只使用管理员文件时可保持命令关闭。各文件位置的只读开关、工具开关、摘要/版本校验和审批继续执行。管理员代理不提供桌面或元数据入口，普通操作也不因代理存在而提升。

## 多项目共享管理员会话

第十一阶段提供 `--admin-broker`。本机所有者一次显式启动，监督器按该次注册表快照登记所有已允许的项目；A、B 可复用这次授权，未允许的 C 不创建管理员代理。每个已授权项目仍使用独立的 root 子代理、私有状态、审批池、任务和工作进程实例；一个项目退出、异常或撤销不会关闭其他项目的代理。普通管理器和工作进程保持原账户权限。该会话不是让多个项目共享同一个策略或任务池。

完整程序、注册表和状态目录及其祖先仍须 root 拥有且不可由普通账户改写，准备规则与下面的单项目入口相同。然后运行：

```sh
sudo /absolute/root-owned/bin/ShellBot --admin-broker \
  --projects /absolute/root-owned/projects.json \
  --owner-uid 1000 --admin-dir /absolute/root-owned/admin-broker-state
```

所有者 UID 必须按目标机器查询，不能把 Linux 的示例 UID 照抄到 Mac。监督器将注册表保存为 root 私有的独立 `grant-*.json` 快照；每个子代理还核对该次授权的项目完整摘要。编辑原注册表不会扩大或改变当前会话；增加项目、改变策略或重新允许已撤销的项目，必须停止本会话，由所有者重新明确授权。程序不设置免密 sudo、不读取或保存密码、不自动弹窗提权。原有单项目 `--admin-agent` 保留。

启动共享会话后再启动/重启对应的普通项目。每个子代理等待其匹配工作进程最多 120 秒，绑定后沿用原有文件、媒体、传输和命令协议。项目停止或暂停、工作进程断开后，子代理退出并取消所属任务、丢弃未提交上传；监督器记录退出，不自动重启子代理，也不重放任务、输入、审批或旧实例。

监督器的本机 owner 通道只接受 root 账户连接。用独立请求文件发送 `{"command":"status"}`，读取本次 `broker_id` 和各项目的 `project_sha256`、`generation`、真实子进程状态及 `host_instance_id`：

```sh
sudo /absolute/root-owned/bin/ShellBot --owner-request /absolute/status.json \
  --data-dir /absolute/root-owned/admin-broker-state
```

项目控制请求只允许 `restart` 和 `revoke`，均须包含 `project_id`、从本次状态读取的 `expected_broker_id`、`expected_project_sha256` 和 `expected_generation`。旧会话、旧代次、摘要不符、未授权项目或未知字段均拒绝。`restart` 只复用最初已经允许且尚未撤销的项目，启动新子代理；所有者还须重启普通工作进程以绑定新实例。`revoke` 立即取消该项目连接和任务，且本次会话不再允许重启它。没有 `grant`、`allow` 或自动重新加载入口。

Ctrl+C 或 `sudo ShellBot --stop --data-dir BROKER_STATE` 撤销全部子代理。监督器异常消失时，私有监督通道关闭，各子代理也会退出并回收任务。状态、快照、审批历史和审计保留。原生管理界面的共享代理配置、启停及管理员审批池将在下一阶段接入；当前使用上述本机命令，真实 sudo/root 行为仍待集中人工验收。隔离的普通 UID 生命周期测试不等于真实 root 或保护文件验收。

## 单项目兼容入口

本地所有者先把完整程序包和注册表副本放到 root 拥有且各级目录不可被普通用户改写的位置；注册表必须与普通项目的配置一致。准备独立 root 私有状态目录。不要更改现有普通 Data 的所有者。然后由所有者在目标机器显式运行：

```sh
sudo /absolute/root-owned/bin/ShellBot --admin-agent \
  --projects /absolute/root-owned/projects.json --project example \
  --owner-uid 1000 --admin-dir /absolute/root-owned/admin-state
```

Mac 的实际 uid 可用 id -u 查询，常见为 501；Linux 示例值不能照抄。sudo 自行向所有者索取密码；ShellBot 不读取、缓存密码，不设置免密 sudo，不自动弹窗提权。管理员程序、注册表、既有 admin-state 及其所有祖先必须 root 拥有、无符号链接且组/其他人不可写；状态目录须为 0700，注册表和程序不能有硬链接或 set-id 位。

代理等待最多 120 秒，随后启动/重启对应的普通项目工作进程。连接通过固定 /run（Mac /private/var/run）下的独占 socket；两端校验实际 UID；代理还校验连接进程确实运行同一份 root 拥有的可执行文件（普通 worker 也须从该安装位置启动）。代理核对完整项目策略（包括命令是否开启）、路径、数据目录、审批方式、资源限制和实例。只授权该配置、该项目、该普通 worker 的一次连接期，不接受远程请求重新注册、切换项目或扩权。没有匹配代理时普通项目仍可运行，但管理员操作明确失败。新旧阶段的策略摘要不同，代理和普通工作进程应使用同一份新程序及一致的注册表。

管理员审批保存在独立的 root 状态；普通审批不生效。返回 pending 后，所有者用 sudo ShellBot --inspect/--approve ID --data-dir ADMIN_STATE 检查并批准，再原样重试包含 approval_id 的请求。审批绑定实际程序、cwd 元数据与实例；stdin 写入同样审批。查看结果、取消和历史按原调用者归属。管理员任务具有 root 账户权限，不受文件根隔离，也可能修改系统或代理自身，只有可信所有者才应授予。

管理员文件操作的私有备份、写入暂存、审批及审计都存放在上述 root 独立状态中。产物每次 open/read/write/commit/status/cancel 都保留 identity:administrator；传输绑定原调用者、项目及本次管理员会话，普通后端不能接续。关闭或撤销代理会丢弃未提交上传，不会创建目标；已完成文件操作不自动回滚或重放。图片/文档沿用固定依赖及读取、像素、页数、解包和输出预算，不运行宏、脚本或外部文档资源。管理员 PDFium 运行库也必须 root 拥有、不可被其他账户改写；解析器在管理员进程内运行，不是独立解析沙箱。

## 命令进程树与资源配额

第十五阶段为每条命令创建独立监督进程，保留原命令 PID，直到清理结束才回收。返回的 process_id 仍是实际命令，supervisor_process_id 是监督进程。正常退出也先清理仍在运行的已识别后代；取消、超时、输出或内存超限、工作进程/任务代理消失都会触发清理。三秒清理期限内没有观察到空集合时，process_tree_cleanup_complete 为 false，并记录错误；不将超时清理算作成功。该字段仅描述当前平台能够识别的集合，不证明任意账户命令均被完全隔离。

Linux 监督进程使用 PR_SET_CHILD_SUBREAPER 接管双重派生、setsid 后的孤儿，以 pidfd 绑定已观察及接管的进程；发送信号不退回可能复用的数值 PID。退出前额外核对当前直接子进程，避免进程扫描与孤儿接管的竞态。需要内核 5.3+ 的 pidfd_open/pidfd_send_signal 支持，以及可读的 /proc；缺失或被系统策略禁止时拒绝命令。进程组仅在原命令仍为本监督进程的未回收子进程时辅助回收。

Intel Mac 通过公开 libproc 按父子关系发现后代，并核对 PID 和出生时间；已观察进程即使更换进程组仍继续核算及回收，发送信号前再次核对身份。公开接口没有 Linux subreaper，已不支持的 kqueue NOTE_TRACK 也不能提供可靠递归追踪。快速双重派生并在采样前脱离祖先的孤儿可能逃离观察；出生时间核对也不是原子的内核句柄。程序能力明确报告此边界，不将其包装为恶意账户命令的沙箱。

两平台名义每 20 毫秒采样一次，按已识别存活后代合计内存：Linux 为 RSS，Mac 为 physical footprint。超出 worker_memory_mb 报告 memory_limit；Linux 普通命令同时保留每进程 RLIMIT_AS。连续三次查询失败则取消并报告 resource_monitor_unavailable；超过采样进程数 1024 则报告 process_limit。收据提供 memory_peak_bytes、memory_accounting 和 process_peak，峰值只包含实际采到的数值。监督进程与工作进程不在此配额中。

合计采样不是分配时硬上限，短时内存峰值或派生数量可超过预算，共享页合计可能重复计算。没有使用需要额外委派权限的 cgroup，也不防御同账户命令主动攻击监督器、隐藏后代或操作其他账户进程。受限命令仍禁止创建子进程并保留原有隔离探针；任务数、输出、超时和账户权限分别生效。

接口依据：[Linux 子进程接管](https://www.man7.org/linux/man-pages/man2/PR_SET_CHILD_SUBREAPER.2const.html)、[pidfd 信号](https://www.man7.org/linux/man-pages/man2/pidfd_send_signal.2.html)、[Apple libproc](https://github.com/apple-oss-distributions/xnu/blob/main/libsyscall/wrappers/libproc/libproc.h)、[Apple kqueue 接口限制](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/sys/event.h)。

Ctrl+C 退出代理、sudo ShellBot --stop --data-dir ADMIN_STATE，或暂停/停止普通项目都会撤销本次连接并取消所属任务；连接关闭不自动重连、重试、提权或降级。后续需所有者显式启动新代理和新项目实例。历史状态保留，不重放旧任务。不要将代理配置为无人确认的永久免密提权服务。

自动验证与真实 sudo/root、系统隐私许可和外部 MCP 人工验收分别记录；本阶段不修改 sudoers、不重启或注销系统。

原生接口依据：[Bubblewrap](https://github.com/containers/bubblewrap/blob/main/bwrap.xml)、[Linux seccomp](https://docs.kernel.org/userspace-api/seccomp_filter.html)。Mac 隔离接口的弃用状态按目标机器的 SDK sandbox.h 与 sandbox_init(3) 文档核对。

若代理遭 SIGKILL 或断电，root 管理员须先确认没有存活代理，再清理其固定 socket 残留；程序不会自动覆盖已有端点。
