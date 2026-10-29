# 图像阅读与缩放（Windows 1.0.4）

`image_view` 读取授权目录内的静态 PNG 或 8 位基线/渐进 JPEG，以原生 MCP image 内容返回。dev.16 在返回原文件前也会实际解码全部像素；PNG 额外校验块 CRC、完整压缩流、Adler-32、精确像素行长度、过滤类型及 Adam7 通道。损坏或超过限制时只返回错误。支持灰度、调色板和透明 PNG；JPEG 色彩转换仍由当前 Windows 内置解码器决定。

## 调用

```json
{
  "workspace_id": "work",
  "path": "图片/照片.jpg",
  "max_dimension": 1600
}
```

`max_dimension` 可省略；提供时须为 128–8192 的整数。长边超过所设值时保持比例缩小，短边向下取整且至少 1 像素，不放大。原文件不修改、不另存输出文件；缩放结果在内存中编码为 PNG。

源文件不超过 16 MiB 且无需缩小时，保持原 PNG/JPEG 字节及元数据。超过 16 MiB 的源文件自动转为 PNG：未指定长边时先使用 4096；若编码仍超过 16 MiB，继续缩小直到满足预算。所设长边是上限，实际结果可能更小，以返回的 width/height 为准。很小的图片含有大量附属元数据时可能仅转码、不改变像素尺寸。

转码保留透明度，使用预乘透明度缩放，输出为 8 位 RGBA PNG。衍生图片不复制原 EXIF、ICC 或文本元数据；本版按存储像素方向处理，不应用 EXIF 自动旋转或 ICC 色彩管理。原样返回的小图仍保留这些元数据。不支持裁剪、旋转、动画或格式写回。

## 大小与结果

| 项目 | 限制或含义 |
| --- | --- |
| 输入文件 | 1 字节至 64 MiB；超过上限时不读取文件正文 |
| 源像素 | 最多 67,108,864，任一边不超过 32,768；请求缩小也不能绕过源限制 |
| 输出编码 | 最多 16 MiB，base64 另计响应开销 |
| width / height | 实际输出像素尺寸 |
| source_width / source_height | 原始存储像素尺寸 |
| size / source_size | 输出编码字节数与源文件字节数 |
| sha256 / source_sha256 | 返回字节与实际源字节的摘要 |
| mime_type / source_mime_type | 输出与源格式，衍生图为 image/png |
| fully_decoded | 全部源像素已实际解码 |
| resized / reencoded | 尺寸是否改变、编码是否重新生成，两者可能不同 |
| exif_orientation_applied | 当前为 false |

host_capabilities 分别报告输入、像素和输出预算。PNG 严格校验的临时原始行数据最多约 128 MiB，之后释放再进入 Windows 像素转换；16 位源转码会降低为 8 位。处理在当前文件工具后端进程内，尺寸限制不等于独立解码器进程隔离或硬超时。JPEG 没有 PNG 式整图校验和，解码器可能容忍部分损坏；成功表示完成像素解码，不证明图片未被改动或视觉内容正确。

## 权限与审批

沿用 `image_view` 的文件工具权限、真实句柄路径检查、目录只读与审批规则，无需桌面或剪贴板权限。`always` 未批准时只返回待办元数据，不读取正文；`workspace_auto` 仅在原目录分类允许时自动读取。工具、目录或执行身份上限不会因缩放扩大。

许可同时绑定源对象版本和 max_dimension 参数。批准后换图片或尺寸会被拒绝；完全相同且未过期的重试复用许可，不重复扣减读取预算。图像逻辑许可按 64 MiB 源读取上限计量，输出的 16 MiB 限制另行执行。撤销、过期或文件变化后不返回旧缓存。管理员文件入口沿用现有代理与审批，本轮未重新做实际 UAC 验收。

专项用独立 Pillow 解码器核对实际像素；Pillow 只用于开发机测试，产品与源码默认构建不依赖 Python/Pillow。真实 MCP 客户端中模型实际看到图片仍需单独验收，见 [M9 交接](HANDOFF_20261011_M9.md)。

接口依据：[微软 WIC 解码器](https://learn.microsoft.com/en-us/windows/win32/api/wincodec/nn-wincodec-iwicbitmapdecoder)、[微软 WIC 缩放示例](https://learn.microsoft.com/en-us/windows/win32/wic/-wic-bitmapsources-howto-scale)、[PNG 规范](https://www.w3.org/TR/png-3/)、[DEFLATE RFC 1951](https://www.rfc-editor.org/rfc/rfc1951.html)。
