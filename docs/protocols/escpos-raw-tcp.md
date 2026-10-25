# ESC/POS RAW TCP 协议

## 1. 状态和启用

这是可选的标准 RAW 打印入口，不使用 IJPD JSON 或 IJPD 二进制帧。

默认固件中：

```text
services.escpos_raw_tcp = false
```

要启用，固件变体必须：

1. 设置 `PrinterConfig.services.escpos_raw_tcp=true`。
2. 配置 `escpos_raw_tcp_port`，默认 `9102`。
3. 注入一个包含 `DataMode::EscPos` 的 Profile。
4. 提供能处理 `submitRaw()` 的真实或 Mock Printhead HAL。

没有 Profile，或 Profile 不支持 ESC/POS 时，应用会强制关闭该服务。

## 2. Wire protocol

```text
TCP byte stream only
```

客户端连接配置端口后，直接发送 ESC/POS 字节：

```text
ESC/POS bytes ... EOF
```

不存在以下内容：

- JSON 控制帧
- `#` 结束符
- IJPD Hello
- IJPD packet header
- CRC32
- LZ4
- ACK/NACK

TCP FIN/EOF 表示该 RAW 任务结束。RAW socket 本身没有响应；完成、错误和取消
状态通过已连接的 IJPD 控制通道广播。

## 3. 行为和限制

- 同时只接受一个 RAW 连接。
- RAW、IJPD TCP raster、页缓存任务互斥。
- 接收块最多按 `1024` 字节读取，再传给 `submitRaw()`。
- 包间空闲超时当前为 `15000 ms`。
- 单任务总超时当前为 `600000 ms`。
- 没有显式 RAW 总字节上限。
- 客户端关闭连接后，Mock/真实 HAL 应完成当前缓冲；设备随后广播完成事件。
- 当前 Profile 的 `escpos_chunk_bytes` 字段不会改变运行时固定块大小 `512`。

## 4. 与 `start_print.mode:"escpos"` 的区别

RAW TCP 是独立的纯字节流协议。虽然 IJPD 控制 schema 保留了
`start_print` 的 `mode:"escpos"`，当前默认镜像没有可用的 IJPD ESC/POS
数据流水线。第三方不要把 `mode:"escpos"` 当作默认可用能力；应以
`capabilities.data_modes` 和 `services.escpos_raw_tcp` 为准。
