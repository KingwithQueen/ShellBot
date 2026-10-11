# PDF / Office 阅读与预览

版本：`0.1.0-dev.17`，M10。日期：2026 年 10 月 11 日。Windows x64 本机验证见 [M10 交接](HANDOFF_20261011_M10.md)。本阶段优先 DOCX / XLSX / PPTX；DOC / XLS / PPT 明确提示转换。

## 工具与单元

| 工具 | 参数与结果 |
| --- | --- |
| document_info | workspace_id、path；返回格式、源大小/SHA-256、unit_count、unit_kind 和前 128 个单元元数据，更多时 units_truncated:true |
| document_read | 加 unit（默认 1）、offset（默认 0）、max_bytes（4–65536，默认 65536）；返回 UTF-8 text、text_size、next_offset、has_more、extraction_truncated |
| document_preview | 加 unit（默认 1）、max_dimension（128–4096，默认 1600）；返回原生 MCP PNG 图片和实际宽高、字节数、摘要、preview_kind、original_layout、content_preview_truncated |

单元从 1 开始。PDF 的 unit 是实际页，最多 4096 页；DOCX 只有一个正文单元，不能用 unit 获取原始 Word 页码；XLSX 按工作簿声明顺序选择工作表，PPTX 按演示文稿声明顺序选择幻灯片，各最多 256 个单元。返回的源摘要覆盖整个输入文件，不是仅覆盖所选页面。

在正常新格式项目中，工具还支持 approval_id 和显式 identity:current_user / administrator。configured 文件范围使用 workspace_id 和相对路径；current_user 文件范围可省略 workspace_id 使用绝对本地盘符路径。三者须加入 policy.tools，不需要启用 desktop 或命令能力。旧单实例入口提供同样的格式处理，但保留原入口权限语义。

## 调用示例

以下为 tools/call 参数，须通过已经固定项目的 MCP 地址或项目请求入口提交：

```json
{"name":"document_info","arguments":{"workspace_id":"workspace","path":"report.pdf"}}
```

```json
{"name":"document_read","arguments":{"workspace_id":"workspace","path":"report.pdf","unit":2,"offset":0,"max_bytes":4096}}
```

```json
{"name":"document_preview","arguments":{"workspace_id":"workspace","path":"book.xlsx","unit":1,"max_dimension":1600}}
```

如果得到 pending，所有者先批准，原调用者再携带相同参数和 approval_id 重试。文字分页只把 offset 改成上次返回的 next_offset；单元、页面大小或源版本改变后重新请求许可。offset 是提取文本的 UTF-8 字节偏移，不是源文件位置；不能切断多字节字符。extraction_truncated:true 表示达到单元提取上限，即使 has_more:false 也可能尚有未提取内容。

## 实际格式行为

PDF 使用固定 PDFium 从内存读取页数、文字和页面像素。文字按 PDF 文本对象顺序返回，不能保证阅读顺序或表格结构；缺失文字映射会影响结果。只有图片的扫描页返回空文本，仍可预览，没有 OCR。PNG 渲染页面几何内容和普通注释，但未创建交互表单环境，不渲染交互表单控件；XFA 和 JavaScript 关闭。系统字体替代也可能影响外观。original_layout:true 表示使用源 PDF 页面几何，不表示与 Acrobat 的所有交互状态完全一致。

DOCX 提取主文档正文中的段落、表格单元文字、换行和制表符，跳过删除修订和字段指令。未接入页眉、页脚、脚注、样式、图片、复杂修订/版面等完整 Word 特性；接受的 XML 包也不代表已验证全部 OpenXML 规范。

XLSX 返回每行 `单元格引用<TAB>存储值`，支持共享/富文本/内联字符串；公式只使用已经保存的缓存结果，不计算公式，不请求外部工作簿。数字、日期序号、布尔值和错误值按原存储文本返回，没有格式化日期或单元格样式。没有保存缓存值的公式可能没有结果；formulas_recalculated:false 表示没有重新计算，不能据此判断工作簿数值已更新。

PPTX 提取选中幻灯片的 DrawingML 文本，保持演示文稿关系顺序；未提取备注、母版内容、图形布局、图表或图片。

Office 预览将选中单元的提取文本绘制成 PNG，图片标题明确标注“Office content preview / 内容预览（非原始排版）”，preview_kind:structured_text、original_layout:false。最多使用前 16 KiB 文本，或在画布容纳不下时裁切，返回 content_preview_truncated:true。它不能复现 Word 分页、Excel 格式或 PowerPoint 版面；需要完整文字时使用 document_read。

不支持旧二进制 Office、DOCM/XLSM/PPTM、其他宏格式或格式转换。需要密码的 PDF 没有密码输入接口；加密 Office 应先准备未加密的受支持副本。文件扩展名须与内容匹配。损坏/不支持的输入返回错误，不把部分处理当成成功。本轮验证了损坏 PDF/Office 拒绝，没有独立真实加密 PDF 夹具验收。

## 权限与资源预算

三工具沿用文件范围、只读、工具掩码、actor、审批及身份策略。always 中连 document_info 也先批准；批准前不解析页数、工作表名、正文或图片，只绑定物理对象元数据。workspace_auto / never 按既有范围规则决定。源变化、跨项目许可、改换参数/单元、撤销或过期均拒绝旧批准。管理员需要独立项目许可、实际 UAC 代理和管理员审批池；普通批准不能授权管理员文档读取。审批工具名存储已扩大，避免 document_preview 的完整名称被截断。

| 项目 | 上限/行为 |
| --- | --- |
| 文档源 | 32 MiB；内存读取，源不落临时文件、不写回 |
| 单元提取文本 | 1 MiB；每次返回最多 64 KiB，显式截断标记 |
| PDF 文本遍历 | 最多 1,048,576 个字符对象 |
| Office ZIP | 最多 2048 项，单项展开 8 MiB，总展开 64 MiB；不支持 ZIP64、多磁盘、加密或其他压缩方法 |
| Office XML | 禁止 DTD / 外部实体解析，深度最多 64、单部件事件最多 1,000,000 |
| XLSX 共享字符串 | 最多 32768 项、累计 4 MiB，单字符串受 1 MiB 文本限制 |
| PNG 预览 | 最多 16,777,216 像素 / 2 MiB 编码；超过编码预算返回错误，可降低 max_dimension 重试 |

Office 在内置 OPC 解码前核对 ZIP 中央/本地记录和展开预算，不向磁盘解包；根据包关系定位实际部件，拒绝选中正文的外部关系。采用精确无宏主文档类型并拒绝 vbaProject.bin，不执行宏、公式或文档外部引用。source_modified:false、macros_executed:false、external_resources_loaded:false 描述这些文档行为，系统字体和 DLL 等系统运行依赖仍可读取。

PDFium 在每个后端进程中串行使用；Office/PDF 解码均在既有文件后端内，未增加独立解码进程或硬超时。输入、展开、事件、像素和输出预算不能保证任意恶意 PDF 的解析内存/CPU 被精确限制，也不构成操作系统级文档解析隔离。当前进程/通信上限沿用原项目资源配置。

大于 512 KiB 回执缓存的 PNG，在同一有效许可、参数和物理版本内重新生成，逻辑读取只计量一次。审计沿用文件操作链，正文和图片不写入审计 JSONL；请求/有限结果保留仍遵循私有 state 规则，见 [审计](AUDIT.md)。

## 构建与依赖

自有代码为 C17；Office 使用 Windows OPC / XmlLite，预览使用 GDI / WIC，不依赖安装 Office、Python、Node.js 或 .NET。PDFium 是 bblanchon 发布的固定 157.0.8086.0 无 V8 / XFA 原生 DLL，发布者独立于上游 Google/Foxit。默认构建将同架构 pdfium.dll 复制到主程序旁，运行时先校验固定 SHA-256 并持有只读句柄；缺失或不符时 PDF 失败，Office 仍可独立使用。不能只复制主程序获得完整 PDF 功能。

DLL 来源、两架构摘要、完整许可及构建参数见 [原生依赖](../third_party/README.md)和 [provenance.json](../third_party/pdfium/provenance.json)。当前仅 x64 产品通过本轮验证，Win32 依赖保留但尚未构建验收。旧候选打包脚本仍绑定 M7 收据，下一候选需更新门槛并包含 DLL 和完整许可，不能复用旧收据。

实现参考：[PDFium 公开文本接口](https://pdfium.googlesource.com/pdfium.git/+/bc5dc39aeb5db6a517d56d23211c30043f52e719/public/fpdf_text.h)、[固定二进制发布](https://github.com/bblanchon/pdfium-binaries/releases/tag/chromium/8086)、[Windows OPC 包结构](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/opc/packages-overview)、[XmlLite GetDepth](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ms752815(v=vs.85))。本机专项、实际 UAC 与独立系统/客户端验收边界详见 [M10 交接](HANDOFF_20261011_M10.md)。
