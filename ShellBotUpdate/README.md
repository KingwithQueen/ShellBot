# ShellBotUpdate

Windows 独立更新器项目，已加入 ShellBot.slnx 和 scripts/build.ps1。C17 入口将更新逻辑作为资源嵌入单个 ShellBotUpdate.exe，由系统 Windows PowerShell 在独立进程中执行本地 ZIP 更新。

运行方式、备份及恢复边界见 [使用说明](../docs/UPDATING.md)。本阶段不实现 URL 下载。
