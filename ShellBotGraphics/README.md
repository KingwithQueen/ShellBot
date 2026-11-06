# ShellBotGraphics

Windows x64 的显式 D3D11 配套程序。只有 `run --instrument-d3d11` 新启动的同用户、同会话、普通 medium 进程装载采集 DLL；没有附着既有进程、管理员、跨用户/会话、登录或解锁接口。经 MCP 使用时，仍需项目已有的普通命令执行许可；观察租约不会自动启用它。

```text
ShellBotGraphics.exe run --instrument-d3d11 --exe C:\App\App.exe --session-dir C:\Work\NewSession --timeout-ms 3600000 -- app-arguments
ShellBotGraphics.exe capture --session C:\Work\NewSession\session.json --output C:\Work\NewFrame.png --after-frame 1 --max-age-ms 2000 --wait-ms 3000
ShellBotGraphics.exe stop --session C:\Work\NewSession\session.json
```

会话目录和 PNG 目标必须新建，不覆盖既有内容。工作目录继承调用方；MCP 调用方可用 `task_start` 的 `path` 明确选择。`run` 立即输出 session 文件与入口 PID，随后持有 Job 到所有受管进程结束；按入口真实退出码结束，并独立报告剩余 Job 数。超时返回 258，显式停止返回 1223；`--timeout-ms 0` 只关闭配套程序自身的超时，外层任务预算仍生效。

使用原子 `PROC_THREAD_ATTRIBUTE_JOB_LIST` 创建挂起子进程，在装载 DLL/恢复之前即已加入本次 Job；若系统或当前 Job 关系不支持，直接报错，不退回先创建后收容。终止托管器时，KILL_ON_JOB_CLOSE 回收本次进程树。Detours 的 helper 回退被拒绝。停止只发送私有命名事件给身份匹配的托管器，不按 session 文件中的裸 PID 杀进程。

会话文件只向当前用户和 SYSTEM 授权，包含随机 nonce、托管器/入口 PID、创建时间、会话和路径。读取及停止核对这些身份。帧映射同样私有，绑定入口及托管器创建时间；header/body 通过安全读取和 seqlock 一致性检查，不信任目标进程宣称的可读页面与尺寸。没有新帧、源帧过旧、提供者未初始化、格式不支持或进程已退出时，不输出 PNG。

`capture` 的 frame 是实际 GPU backbuffer 复制代次，时间戳是源复制时间；`--after-frame` 要求更大的源代次，`--max-age-ms` 限制读取时源帧龄。PNG 缩放/编码及传输另耗时间，回执保留原时间戳和最终 age，调用方应核对。像素来自 Present 前的纹理，不能冒充 DWM 已显示的新帧，不能作为物理坐标输入依据。应用若停止绘图，配套程序无法生成它尚未绘制的内容。

本版只采集入口进程中第一个由 `D3D11CreateDeviceAndSwapChain` 创建的窗口交换链及 `Present`；支持非 MSAA、单层、单 mip 的 BGRA/RGBA 8-bit UNORM/sRGB。每次获取/释放 backbuffer，窗口尺寸或格式改变时重建 staging，不阻碍正常 ResizeBuffers。帧默认上限 256 MiB，覆盖 7680×4320 BGRA 8K；映射只预留地址，按实际帧提交内存。`--max-frame-bytes` 可降低配额；PNG 默认最长边 1600，`--max-dimension` 可提高至 16384，仍受帧字节配额约束。

尚不覆盖 Present1、现代 Factory CreateSwapChainForHwnd/Composition、其他 GPU API、HDR/10-bit、MSAA、多进程 GPU 子进程或多个交换链。不会热卸载 DLL；它随目标进程结束。同步 staging Map 会增加 GPU 延迟，适合明确选择此模式并完成实际应用验收的场景；复制失败仍原样转发原 Present，不自动重放输入。应用自身仍可能激活窗口，配套程序不发送物理键鼠或请求聚焦。

`hook-status.jsonl` 与失败回执保留 DLL 初始化、Create/Present hook 安装、调用次数、实际 API 返回值及阶段。创建窗口句柄不代表 GPU 或帧映射已经就绪；`capture --wait-ms` 会等待真实帧。状态阶段：1 初始化、2 Create hook 就绪、3 调用创建、5 Present hook 就绪、6 帧复制完成、7 错误。

从 `ShellBotGraphics.vcxproj` 构建，自动引用 `Capture.vcxproj` 和 `Detours.vcxproj`。C17 自有代码配合固定的微软官方 Detours v4.0.1（MIT）源构建，原始字节及来源记录在 `third_party/detours/provenance.json`，不含 OBS 源码。Win32 配置明确跳过此配套项目；其余 ShellBot Win32 主程序构建保持独立。

依据：[微软 Detours 使用说明](https://github.com/microsoft/Detours/wiki/Using-Detours)、[创建时装载说明](https://github.com/microsoft/Detours/wiki/DetourCreateProcessWithDllEx)。此 API instrumentation 路径不保证任意应用兼容，必须分别核对真实新帧、操作结果和资源开销。
