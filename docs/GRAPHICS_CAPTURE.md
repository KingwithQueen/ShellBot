# Windows 锁屏时的 GPU 取图（1.1.7）

WGC/PrintWindow 适合普通窗口，但不能保证锁屏时的 GPU 内容已经刷新。1.1.7 提供两条不需要虚拟机的显式配套路径：Chromium 页面的 DevTools 截图，以及新启动的 x64 D3D11 程序的提交前纹理捕获。它们不登录、不解锁、不切换会话，也不自动修改现有应用或启用调试端口。

这些程序可通过已有 task_start 运行，需要项目已允许普通命令执行。普通桌面工具的租约、审批和人工暂停继续独立生效；获取图片不会授权聚焦或物理键鼠。后台控制仍优先使用新观察对应的 UIA pattern，并读取状态确认结果。

## Chromium / Electron 页面

对明确启用了本机调试端口的自动化实例，运行配套脚本：

```powershell
powershell.exe -NoProfile -File .\docs\tools\ShellBotChromiumCapture.ps1 -Endpoint http://127.0.0.1:9222 -TargetId 页面ID -OutputPath D:\Work\new-frame.png
```

Endpoint 和页面 ID 必须由调用方明确指定；只接受本机回环地址，并核对监听进程的账户、会话和权限。脚本拒绝重定向、不扫描用户配置、不启动或重启浏览器，不提供任意脚本执行或输入接口。输出必须为新文件。截图成功后可通过 image_view 查看。

调试端口须在启动目标应用时由所有者明确启用。Chrome/Edge 自动化应使用独立的用户数据目录，例如 `--remote-debugging-address=127.0.0.1 --remote-debugging-port=9222 --user-data-dir=D:\Work\AutomationProfile`；不要复制或重命名正在使用的默认配置目录。页面 ID 可从该显式端点的 `/json/list` 得到。Electron 是否开放相同协议取决于应用的启动设置；未开放的实例不能由本脚本自动改造。

脚本从页面 surface 取得当前内容，包含页面内的 WebGL，不包含浏览器标题栏或其他原生窗口。对导航、脚本加载和应用状态应分别等待；网页尚处于 about:blank 或 loading 时，不能把首次返回的空页面当作目标内容。接口依据 [Page.captureScreenshot](https://chromedevtools.github.io/devtools-protocol/tot/Page/#method-captureScreenshot)。Chrome 的独立配置要求见 [官方说明](https://developer.chrome.com/blog/remote-debugging-port)。

## 新启动的原生 D3D11 程序

使用 ShellBotGraphics.exe 的显式启动采集模式：

```powershell
.\ShellBotGraphics.exe run --instrument-d3d11 --exe D:\Apps\Example.exe --session-dir D:\Work\GraphicsSession --timeout-ms 600000 -- 参数
```

session-dir 必须是新目录。启动器在同一用户、同一会话以普通权限新建 x64 目标进程，加载随包采集模块并托管整个 Job；不会附着用户已经运行的程序。它输出目标 PID 和受保护的 session.json 路径。模块在应用调用 Present 之前读取真实纹理，原始 Present 仍照常执行并记录独立返回值。

获取一张新图片：

```powershell
.\ShellBotGraphics.exe capture --session D:\Work\GraphicsSession\session.json --output D:\Work\frame-1.png --wait-ms 3000 --max-age-ms 2000 --max-dimension 1600
```

后续动作完成后，把上次回执的 frame 传入 `--after-frame`，要求帧号必须更大：

```powershell
.\ShellBotGraphics.exe capture --session D:\Work\GraphicsSession\session.json --output D:\Work\frame-2.png --after-frame 1 --wait-ms 3000 --max-age-ms 2000
.\ShellBotGraphics.exe stop --session D:\Work\GraphicsSession\session.json
```

capture 等待初始化和稳定快照，检查帧号、年龄、PID/进程创建身份、账户、会话和私有文件权限。没有新帧、帧太旧或格式不支持时明确失败，不用旧图片冒充新图，也不会覆盖已有输出。stop 只向经过身份核对的本启动器发出停止信号，由启动器清理自己托管的 Job，不按会话文件中任意 PID 杀进程。目标退出、超时和主动停止在最终回执中分别报告。

当前只支持目标进程内经 D3D11CreateDeviceAndSwapChain 创建的首个窗口交换链、Present、BGRA/RGBA 8 位非 MSAA 内容。不覆盖 Present1、现代工厂创建交换链、D3D12、OpenGL、Vulkan、GPU 子进程或受保护内容；普通截图不会自动降级为注入式采集。自定义引擎可能停止绘制，此时应处理“无新帧”，不能自动重放操作。此路径会增加 GPU 读回开销，应先对实际应用验证。

源码为 C17；第三方 Microsoft Detours 固定版本、MIT 许可及来源校验位于 third_party/detours，随包声明位于 licenses。实现参考提交前捕获的公开设计，没有复制 OBS 的 GPL 源码。

## 实测范围

2026-11-06，Z620 的同一用户会话保持 disconnected/locked，前台为空，输入桌面查询错误 5：

- 原生 D3D11 探针的真实纹理已经变蓝、Present 返回 DXGI_STATUS_OCCLUDED 时，PrintWindow 仍返回旧橙色；改变 flags、RedrawWindow、DwmFlush 均未修复单次 Present 的旧面。连续/重复 Present 可使该探针的部分合成面更新，不能从外部强迫任意应用采取这种渲染方式。
- 显式提交前采集原型在同一锁屏状态取得递增的三帧，橙→蓝→橙各 37120 个真实 GPU 像素；探针自身的独立 staging 读回关闭，原始 Present 状态保留。
- Edge 的真实 NVIDIA Quadro K6000 D3D11/WebGL 页面，通过本地 DevTools 输入触发两次测试按钮，三张页面 PNG 均为正确新色，旧色像素为零。该测试只访问自建页面与私有配置，不代表所有 Electron 应用已验收。

原型、正式候选与包内程序的最终核对范围见交付收据。普通 PrintWindow 回执继续明确 `frame_freshness.status:unverified` 和 `cached_surface_possible:true`；WGC 附合成帧时间/年龄，时间戳本身也不证明应用操作成功。
