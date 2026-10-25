# tools/ —— 主机侧开发工具（仅 Python 标准库，不入固件产物）

## make_packets.py

测试用二进制分包生成器，实现：

- **LZ4 Block 编码器**（贪心哈希匹配）：仅用于生成测试向量与回环自检；
- **LZ4 Block 解码器**：语义与固件 `components/core/src/lz4_block.cpp` 一致，用于交叉对照；
- **帧构造**：Hello（12B）+ 分包帧（协议 §6，含 Little Endian、nominal/last 包长规则、
  存储兜底（`clen==ulen` 即原始数据）、可选 CRC32）；
- **数据集生成**：random / runs / zeros / mixed 四种图案。

### selftest（回环 + 帧流重建 + 可选固件解压器交叉验证）

```bash
python3 tools/make_packets.py selftest
# 交叉验证（需先构建 host_tests，产出 lz4_check）：
python3 tools/make_packets.py selftest --decoder host_tests/build/lz4_check
```

### gen（生成任务流文件，供手工测试 / Phase 6 send_job.py 参考）

```bash
python3 tools/make_packets.py gen --out job.bin --bytes 120000 --packets 30 \
    --compression lz4_block --crc32 --pattern mixed --token 0x12345678
```

输出文件布局：`Hello(12B) | Frame0 | Frame1 | ... | FrameN-1`。

## send_job.py（Phase 6 提供）

控制+数据双通道模拟客户端（socket/struct/json/zlib.crc32）。
设备在取消或致命错误时可能先关闭 TCP `9101`；客户端现将发送期间的
`BrokenPipeError`/连接重置视为终态路径并继续等待控制通道的 `print_finished` 事件。

## airprint_sample.py

AirPrint 黄金样本清单工具。它只保存捕获 body 的长度、SHA-256、document
范围和人工确认的元数据，不解析 URF/Apple Raster。`import` 会把原始 body、
可选 HTTP/IPP metadata、人工准备的像素参考文件复制到一个可复核的 bundle，
并为每个 artifact 记录长度和 SHA-256。

```bash
python3 tools/airprint_sample.py create \
    --body capture.ipp --output sample.json \
    --sample-name mono-a4-001 --document-format image/urf \
    --document-offset 1234 --document-bytes 4096

python3 tools/airprint_sample.py check --manifest sample.json --body capture.ipp

python3 tools/airprint_sample.py import \
    --body capture.ipp --output-dir samples/mono-a4-001 \
    --sample-name mono-a4-001 --document-format image/urf \
    --document-offset 1234 --document-bytes 4096 \
    --document expected-pixels-or-raw-document.bin \
    --analysis analysis.json \
    --pcap capture.pcap \
    --http-capture capture.http \
    --ipp-attributes ipp-attributes.txt \
    --capture-metadata capture-metadata.json \
    --expected-pixels expected-pixels.bin

python3 tools/airprint_sample.py check --bundle samples/mono-a4-001
python3 tools/test_airprint_sample.py
```

bundle 内固定保存 `manifest.json`、`ipp_body.bin` 和提供的 evidence artifacts。
样本工具不会解析 Apple Raster/URF，也不会从 HTTP/IPP metadata 推断
`document_offset`、页面尺寸、DPI、stride 或颜色布局。只有在 macOS/iOS 抓包、
IPP metadata、document offset/length 和预期像素都人工确认后，才允许把 manifest
标记为 `expected_values_known=true`，再进入具体 decoder 实现。

`tools/airprint_pcap.py` 可从 Ethernet pcap 重组完整 chunked `Print-Job` 流，并
提取 IPP document range；它只处理 TCP/HTTP framing 和 end-of-attributes boundary，
不会解释 URF/Apple Raster 字段：

```bash
python3 tools/airprint_pcap.py extract \
    --pcap evidence/system-query-002/airprint-printjob.pcap \
    --output-dir evidence/system-query-002/extracted
```

`tools/airprint_urf_analyze.py` 只按公开 CUPS Apple Raster framing 和 modified
PackBits 行规则分析已提取 document，输出 page descriptor、行统计和 CRC32；它不是
设备 decoder：

```bash
python3 tools/airprint_urf_analyze.py compare \
    --document evidence/golden-samples/urf-photo-preview-707738/document.bin \
    --document evidence/golden-samples/urf-photo-preview-722531/document.bin \
    --output evidence/golden-samples/urf-analysis-compare.json
```

## airprint_query.py

向显式 AirPrint service 发送一个原始 `Get-Printer-Attributes` 请求，保存完整
HTTP/IPP evidence。工具只解析 HTTP 状态和 IPP 8 字节消息头，不解析属性含义，
也不解析任何 raster 数据。

```bash
python3 tools/airprint_query.py \
    --host ijpd-xxxx.local \
    --printer-uri ipp://ijpd-xxxx.local/ipp/print \
    --output-dir evidence/query-001 \
    --requested-attribute printer-name \
    --requested-attribute operations-supported
```

输出目录包括：

- `request.http` / `request.ipp`：发送的完整 HTTP 和 IPP body。
- `response.http` / `response.ipp`：设备返回的完整 HTTP 和 IPP body。
- `metadata.json`：目标、时间、状态、请求 ID、长度和 SHA-256。
