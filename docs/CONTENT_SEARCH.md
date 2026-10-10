# 文本内容搜索

日期：2026 年 10 月 6 日。版本：`0.1.0-dev.4`。

fs_search 保留默认的文件名搜索，增加 mode:"content"。内容模式对授权目录中的文本执行区分大小写的单行字面量包含搜索，支持 UTF-8 与带 BOM 的 UTF-16 LE/BE，支持中文与 emoji；不把 query 当作正则表达式。

```json
{
  "workspace_id": "workspace",
  "path": ".",
  "query": "需要查找的文字",
  "mode": "content",
  "recursive": true,
  "max_results": 50,
  "max_file_bytes": 262144,
  "max_total_bytes": 4194304
}
```

每个命中行返回一条结果，包含相对路径、从 1 开始的 line、从 1 开始的 column_byte、encoding、excerpt、excerpt_truncated、file_truncated。column_byte 是解码为 UTF-8 后该行的字节列号，不是字符序号。LF、CRLF 与单独 CR 均计为换行。同一行出现多个匹配只返回一条。

excerpt 最多 1024 个 UTF-8 字节，在命中位置周围截取，保留完整字符。excerpt_truncated 表示行片段截断；file_truncated 表示只扫描了文件前缀。顶层 truncated 表示结果、时间、深度、条目、读取量或输出预算使查询不完整，不能从零命中推断整个目录没有匹配。

默认单文件读取 256 KiB、总计读取 4 MiB；可分别设为 4～1048576 与 4～16777216 字节。max_results 为 1～100。每次最多检查 1000 个目录条目、递归 8 层、结果预算 128 KiB，并在操作之间检查 2 秒时间预算；单次文件系统调用仍可能阻塞，时间预算不是系统硬超时。

打开文件后校验其最终路径，越界文件和不可读文件不会返回内容；递归跳过重解析目录。被读取前缀中检测到的二进制数据与无效编码跳过，并累计 skipped_files。files_read、bytes_read、partial_files、skipped_directories 与 skipped_reparse_directories 提供实际扫描信息。尚未检测到的文件后缀不参与格式判断。

本功能按请求读取，不建立整盘索引，不修改文件。PDF、Office、GBK 等格式尚未接入文本转换后端。
