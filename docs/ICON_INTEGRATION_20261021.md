# ShellBot 1.0.0 图标接入

用户指定原图：`assets/branding/shellbot-v1/shellbot-icon.png`。本轮保留原图和透明背景，按原比例缩放，不修改造型或颜色。版本继续为 **1.0.0**。

## 资源与使用位置

- `scripts/make-icon.ps1` 使用 Windows 的图像接口生成 `assets/branding/shellbot-v1/shellbot.ico`，包含 16、20、24、32、40、48、64、96、128、256 像素的 32 位 RGBA PNG 帧。再次运行该脚本可从原图重建 ICO。
- 原 PNG SHA-256：`a7f95ed9be861a56d0d0073bda02144c6434b132bc0626eb23131b44cd26a055`。
- 新 ICO SHA-256：`c75952d0fdadf99a49d9230df291a9f98db1084c97ee145fd41cbd8dc009e5d3`。
- `WinShellBot/app/resources.h` 定义资源 ID 101，`version.rc` 嵌入图标；ShellBot.exe、WinShellBot.exe 和 ShellBot-SandboxProbe.exe 使用相同资源。
- `platform/app_icon.h` 统一加载已嵌入的图标，供管理窗口、项目/连接/数据/权限/OAuth 表单，以及管理和桌面控制托盘使用。只有资源加载失败时才回退系统图标。
- 运行期沿用共享图标句柄，不在周期刷新和托盘重试时创建待释放的新图标。参见 [Microsoft LoadIconW](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-loadiconw) 的缓存行为说明。

图标已经在 EXE 内，不需要运行时读取外部 ICO 或原 PNG。原有项目、凭据、权限和数据格式保持。

## 构建、验证与交付

本轮基线为 `build/baseline-icon-1.0.0-20261021-232345/manifest.json`；本轮记录独立放在 `build/icon-release-1.0.0`，不改前一轮冻结的 `build/release-1.0.0`。

`icon-verification.json` 逐字节比较 ICO 与两个构建配置中三个 EXE 的资源，共 60 帧，逐帧调用 Windows 原生解码并核对尺寸；同时检查透明通道。尺寸预览为同目录 `icon-sizes.png`。真实窗口截图来自本轮布局检查目录。

发布门禁要求原有 25 份当前程序回归、Release 的桌面控制托盘注册及两次原生恢复回调分项，以及当前 PNG、ICO 和六个 EXE 的图标验证。`package-gates.json` 独立记录错误报告拒绝检查。完整结果以本轮 `acceptance.json`、`package-receipt.json`、`extracted-runtime.json` 和 `final-verification.json` 为准。

额外的 M8 全量桌面检查在已完成托盘恢复分项后，被 Windows 前台焦点限制阻止，未发送输入。`release-desktop-control.json` 保持 failed；`release-desktop-tray.json` 仅引用其中 6 个已经通过的托盘分项，绑定原始报告摘要并明确排除完整桌面输入验收。不把全量失败报告改标为通过。

首次 Release 核心回归中，一个测试文件的独占打开被拒绝，原始失败和对应测试目录保留。未改变图标以外的文件锁/权限逻辑，未逐请求重试；相同二进制在新测试目录的完整核心复验通过。首次失败根因没有独立定位，不将其改写成已修复问题。

带图标的新包：`dist/ShellBot-1.0.0-windows-x64-branding.zip`。原 `dist/ShellBot-1.0.0-windows-x64.zip` 保留，SHA-256 仍为 `87faece67db5a68707e5622809b5133ec08430004c4f6f55e2cb44e41736cd44`。新包摘要以自身 `.zip.sha256` 和新收据为准。实际解压后检查 GUI 入口、两种布局、全文件摘要和所属进程清理；网络检查继续使用既有排除路径。

本轮未运行真实客户端或实际 UAC，没有重启 Explorer，也没有修改图标缓存或用户网络设置。源码基于 main 的 `1eea3ae9d4fc870f071773aeae0bc20cc1a1863a`，本轮不自动提交或推送。
