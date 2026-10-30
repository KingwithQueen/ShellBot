# ShellBotUpdate 本地 ZIP 更新器

把 `ShellBotUpdate.exe` 放在 ShellBot 安装根目录，与 `ShellBot.exe`、`WinShellBot.exe` 同级。双击选择已经下载的 ZIP，或运行：

```powershell
.\ShellBotUpdate.exe "E:\Downloads\ShellBot-1.0.4-windows-x64.zip"
```

安装位置由更新器自身目录决定。找不到相邻核心 `WinShellBot.exe` 时立即报错。本阶段仅支持本地 ZIP，不下载网址，也不删除用户提供的 ZIP。

ZIP 可平铺或包含一个顶层安装包文件夹，必须有四个运行文件：`ShellBot.exe`、`WinShellBot.exe`、`ShellBot-SandboxProbe.exe`、`pdfium.dll`，以及发布包格式的 `SHA256SUMS.json`。逐文件摘要用于检查损坏，不是发布者签名，应使用可信安装包。

更新范围仅为固定运行文件、可选的更新器、产品说明及 docs/licenses；拒绝包内的 Data、位置设置和其他未允许文件。已有 Data、`ShellBot.storage.json`、自定义配置和工作目录保留。拒绝越界路径、链接、重复文件、Windows 保留名及未列入摘要清单的文件。展开预算为单文件 4 GiB、总计 16 GiB、100000 个文件，另需完整旧文件备份空间。

更新器启动系统自带 Windows PowerShell 独立进程后退出，从而支持替换更新器自身。全部文件解压校验通过后，按位置设置中的 Desktop/Manager 请求当前目录的管理器正常退出；不按名称强杀进程，也不停止其他安装。仍在运行的托盘、自定义管理器、服务或文件占用需要先正常退出再重试。安装完成后不自动重新启动 ShellBot。

替换前备份所有待覆盖文件，写入后再次校验。普通异常会尝试回滚已替换文件；恢复失败标记为 `recovery_required` 并保留错误及备份位置。多文件更新不是断电原子事务，强制退出或断电后应检查记录并从备份恢复。

每次更新记录保存在根目录 `.shellbot-update/job-…/result.json`，原文件保存在同目录 backup 中。成功后删除本次解压缓存，备份、记录及原 ZIP 保留。URL 下载和下载缓存删除留待后续开发。

交互模式在完成或失败后弹出结果。自动化可用 `--quiet "ZIP路径"` 禁用提示框：启动退出码 0 只表示更新任务已启动，标准输出 `JOB=…` 给出记录目录，最终结果以 result.json 的 status 为准；启动检查失败返回 2。依赖 Windows 自带的 Windows PowerShell 5.1 / .NET Framework，不需要 Python 或第三方解压程序。
