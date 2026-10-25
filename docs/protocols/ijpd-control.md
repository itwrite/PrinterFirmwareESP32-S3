# IJPD 控制协议 V1

## 1. 作用和传输方式

IJPD 控制协议负责设备查询、任务控制、状态查询和事件通知。它不是 HTTP，
而是一个以 `#` 分隔的 UTF-8 JSON 字节流。

控制协议可通过以下三种承载使用：

| 承载 | 默认状态 | 说明 |
|---|---|---|
| TCP `9100` | 默认启用 | 最多两个并发控制连接 |
| BLE GATT | 默认启用 | 写特征发送，Notify 特征接收 |
| MQTT | 默认关闭 | 配置后使用 `/command` 和 `/event` topic |

三种承载进入同一个 ControlPlane 和 CommandRouter，命令语义一致。`start_provisioning`
可通过任一控制承载请求切换 Wi-Fi 配网；切换不依赖 AirPrint。

## 2. 控制帧

```text
JSON UTF-8 body + ASCII '#'
```

示例：

```json
{"msg_id":"m-001","cmd":"get_device_info","data":{}}#
```

规则：

- 第一个裸 `#` 是帧结束符。
- JSON 字符串中不能出现裸 `#`；逻辑字符 `#` 必须写为 `\u0023`。
- 空帧 `#` 被静默忽略。
- 控制请求正文最大 `2048` 字节，不含结尾 `#`。
- 半帧 `3000 ms` 内没有新字节时，设备丢弃半帧并返回 `FRAME_TIMEOUT`。
- 超长帧会被丢弃到下一个 `#`，返回 `FRAME_TOO_LARGE`。
- TCP/BLE 字节可以任意拆分或粘连，客户端必须按 `#` 拆帧。
- 设备响应最大约 `6144` 字节，客户端接收缓冲应至少准备 `6144` 字节。

## 3. JSON 入站限制

设备实现的是有界 JSON 子集：

- 根请求必须是 object。
- 最大嵌套深度 `4`。
- 最大 token 数 `64`。
- 每个解码后的字符串最大 `128` 字节。
- 整数为有符号 `int64`。
- 不支持浮点数、指数记法、尾逗号、重复键和非法 UTF-8。
- 字符串支持标准 JSON 转义和合法 UTF-16 surrogate pair。
- 顶层必须且只能有 `msg_id`、`cmd`、`data` 三个字段。
- `data` 必须是 object；未知业务字段会被拒绝。
- `msg_id` 长度 `1..40` 字节。

## 4. 请求和响应

请求：

```json
{
  "msg_id": "m-001",
  "cmd": "get_device_info",
  "data": {}
}#
```

成功响应：

```json
{
  "msg_id": "m-001",
  "cmd": "get_device_info",
  "data": {
    "success": true,
    "result": {},
    "error": null
  }
}#
```

失败响应：

```json
{
  "msg_id": "m-001",
  "cmd": "start_print",
  "data": {
    "success": false,
    "result": null,
    "error": {
      "code": "INVALID_ARGUMENT",
      "message": "..."
    }
  }
}#
```

无法解析出 `msg_id` 的传输层错误使用 `"msg_id":null` 和
`"cmd":null`。

## 5. 命令

### 5.1 `get_device_info`

无参数。客户端应在首次连接后调用此命令。

重点结果字段：

```json
{
  "device": {
    "device_id": "IJPD-71D5D0",
     "model": "RICOH-MH5440-MOCK",
    "fw_version": "0.3.0",
    "protocol_version": "1.0",
    "connections": ["ble", "wifi_tcp"],
    "hal_mode": "mock"
  },
  "printhead": {
     "dpi": 600,
     "print_width_px": 320,
     "pixel_format": "cmyk8",
    "is_configured": false
  },
  "capabilities": {
    "printer_type": "inkjet",
    "color": true,
    "data_modes": ["raster"],
    "compressions": ["none", "lz4_block"],
    "integrity": ["none", "crc32"],
    "paper_feed": true,
    "cutter": false,
    "mock_print": true,
    "services": {
      "wifi": true,
      "ble": true,
      "ijpd_tcp": true,
      "escpos_raw_tcp": false,
      "mqtt": false
    },
    "service_ports": {
      "escpos_raw_tcp": null
    },
    "sensors": []
  },
  "limits": {
    "max_control_frame": 2048,
    "max_packet_compressed": 65535,
    "max_packet_uncompressed": 65535,
    "max_job_bytes": 67108864,
    "max_total_packets": 65535,
    "queue_depth": 6,
    "control_port": 9100,
    "data_port": 9101
  }
}
```

设备还返回 `commands` 元数据数组，包含每个命令的 `supported`、参数类型、
范围和枚举值。实际值应以设备返回为准。

### 5.2 `start_provisioning`

请求设备切换到独立的 SoftAP 配网门户。该命令不依赖 AirPrint；默认固件和
AirPrint probe 都可提供。只有在没有活动打印任务、数据传输或 RAW 任务时才允许。

请求：

```json
{"msg_id":"m-provision","cmd":"start_provisioning","data":{}}#
```

成功响应中的 `result`：

```json
{
  "status": "portal_starting",
  "ssid": "IJPD-Setup-XXXX",
  "ip": "192.168.4.1",
  "port": 80
}
```

控制响应发送完成后，设备停止当前 STA 网络并启动 SoftAP。客户端随后连接返回的
热点，并访问 `http://192.168.4.1` 的配网 HTTP API。活动任务期间返回
`JOB_ALREADY_RUNNING`。

### 5.3 `start_print`

创建单个打印任务。一次只能有一个活动任务。

请求参数：

| 字段 | 类型 | 必填 | 约束 |
|---|---|---:|---|
| `job_id` | string | 是 | `1..40` 字节，仅 `[A-Za-z0-9_-]` |
| `total_bytes_uncompressed` | integer | 是 | `1..67108864` |
| `total_packets` | integer | 是 | `1..65535` |
| `compression` | enum | 是 | `none` / `lz4_block` |
| `integrity` | enum | 否 | 默认 `none`；`none` / `crc32` |
| `mode` | enum | 否 | 默认 `raster`；`raster` / `escpos` |
| `transport` | enum | 否 | 默认 `tcp`；`tcp` / `mqtt` |
| `bytes_per_row` | integer | raster 时是 | `1..65535`，且乘以 `total_rows` 等于总字节数 |
| `total_rows` | integer | raster 时是 | `>=1` |
| `pixel_bits` | integer | 否 | 显示提示；通常 `1` 或 `8` |
| `image_planes` | integer | 否 | 显示提示；默认 `1` |
| `channel_order` | string array | 否 | `1..8` 个通道名，需与平面数一致；见下表 |
| `cutter_after_finish` | boolean | 否 | 默认 `false` |

`channel_order` 支持的名称为：

```text
black, cyan, magenta, yellow, lightCyan, lightMagenta,
gray, lightGray, red, green, orange, violet, white, clear, custom
```

名称区分大小写。未提供时使用设备默认的平面顺序。

当前默认固件使用 RICOH MH5440 logical CMYK8 Mock，第三方应优先使用
`mode:"raster"`、`bytes_per_row:1280` 和 `image_planes:4`。虽然 schema 保留
`mode:"escpos"`，当前默认镜像没有可用的 IJPD ESC/POS 数据流水线；
原始 ESC/POS 应使用独立的 [RAW TCP 协议](escpos-raw-tcp.md)，并且该服务需要
显式启用。

Raster 示例：

```json
{
  "msg_id": "m-002",
  "cmd": "start_print",
  "data": {
    "job_id": "job-001",
    "total_bytes_uncompressed": 4800,
    "total_packets": 10,
    "compression": "none",
    "integrity": "crc32",
    "mode": "raster",
    "transport": "tcp",
    "bytes_per_row": 48,
    "total_rows": 100,
    "cutter_after_finish": false
  }
}#
```

TCP 成功响应示例：

```json
{
  "msg_id": "m-002",
  "cmd": "start_print",
  "data": {
    "success": true,
    "result": {
      "job_id": "job-001",
      "status": "waiting_data_conn",
      "data_port": 9101,
      "data_token": 305419896,
      "nominal_packet_bytes": 480,
      "compression": "none",
      "integrity": "crc32",
      "mode": "raster",
      "data_connect_timeout_ms": 10000,
      "limits": {
        "max_packet_uncompressed": 65535,
        "max_job_bytes": 67108864
      }
    },
    "error": null
  }
}#
```

包长派生规则：

```text
nominal_packet_bytes = ceil(total_bytes_uncompressed / total_packets)
last_packet_bytes = total_bytes_uncompressed
                     - nominal_packet_bytes * (total_packets - 1)
```

除最后一包外，每包解压后长度必须等于 `nominal_packet_bytes`；最后一包必须
等于 `last_packet_bytes`。

MQTT transport 的响应状态为 `waiting_mqtt_chunks`，并额外返回
`mqtt_chunk_topic`，见 [MQTT 文档](mqtt.md)。

### 5.4 `cancel_print`

`job_id` 可选。缺省取消当前任务：

```json
{"msg_id":"m-003","cmd":"cancel_print","data":{}}#
```

成功响应通常是 `status:"cancelling"`。最终结果通过后续 `print_finished` 事件
报告，不要把该响应当作已完成。

### 5.5 `get_print_status`

无参数。返回当前或最近任务：

```json
{
  "job_id": "job-001",
  "status": "receiving",
  "generation": 3,
  "stats": {
    "packets_received": 2,
    "packets_decoded": 1,
    "bytes_decoded": 480,
    "bytes_printed": 240,
    "queue_watermark": 1,
    "queue_watermark_max": 2,
    "throughput_bps": 48000,
    "crc_errors": 0,
    "decode_errors": 0
  }
}
```

状态通常包括：`idle`、`preparing`、`waiting_data_conn`、`receiving`、
`draining`、`finalizing`、`cancelling`、`completed`、`done_with_warning`、
`cancelled`、`error`。

### 5.6 `feed_paper`

```json
{"msg_id":"m-004","cmd":"feed_paper","data":{"distance_um":10000}}#
```

`distance_um` 范围为 `1..254000`。当前返回 Mock 走纸结果：

```json
{"moved_um":10000}
```

### 5.7 `cut_paper`

无参数。当前默认 RICOH Mock 没有切纸能力，`supported` 为 `false`，调用返回
`UNSUPPORTED`。

## 6. 打印事件

事件广播给当前已注册的 TCP、BLE 和 MQTT 控制接收端：

```json
{
  "msg_id": "evt-0001",
  "cmd": "print_progress",
  "data": {
    "success": true,
    "result": {
      "job_id": "job-001",
      "status": "receiving",
      "packets_received": 5,
      "packets_decoded": 4,
      "bytes_decoded": 1920,
      "bytes_printed": 960,
      "queue_watermark": 2,
      "queue_watermark_max": 3,
      "throughput_bps": 48000
    },
    "error": null
  }
}#
```

终态命令为 `print_finished`，`status` 为 `completed`、`done_with_warning`、
`cancelled` 或 `error`。终态结果还包含 `bytes_printed`、`duration_ms`、
`decoded_bytes` 和 `decoded_crc32` 等字段；失败时使用 `error.code`。

## 7. 错误码

| 错误码 | 含义 |
|---|---|
| `INVALID_JSON` | JSON 语法或 JSON 子集约束错误 |
| `INVALID_ARGUMENT` | 参数类型、范围、组合或未知字段错误 |
| `UNKNOWN_COMMAND` | 命令不在白名单 |
| `FRAME_TOO_LARGE` | 控制正文超过 `2048` 字节 |
| `FRAME_TIMEOUT` | 控制半帧超过 `3000 ms` 未完成 |
| `PACKET_OUT_OF_ORDER` | 数据包重复、乱序或缺失 |
| `PACKET_LENGTH_INVALID` | 数据包长度或总长度不正确 |
| `PACKET_CRC_MISMATCH` | CRC32 校验失败 |
| `DECOMPRESSION_FAILED` | LZ4 解压失败 |
| `DECOMPRESSED_LENGTH_MISMATCH` | 解压结果长度不符 |
| `QUEUE_TIMEOUT` | 队列/发送等待超时 |
| `PSRAM_UNAVAILABLE` | PSRAM 不可用 |
| `OUT_OF_MEMORY` | 内存分配失败 |
| `JOB_ALREADY_RUNNING` | 已有活动任务 |
| `NO_ACTIVE_JOB` | 当前没有任务 |
| `DATA_CONNECTION_LOST` | 数据连接意外断开 |
| `DATA_HANDSHAKE_FAILED` | 数据 Hello 校验失败 |
| `HAL_NOT_CONFIGURED` | HAL 未配置 |
| `HAL_FAULT` | HAL 执行失败 |
| `UNSUPPORTED` | 当前设备不支持该能力 |
| `JOB_CANCELLED` | 任务被取消 |
| `JOB_TIMEOUT` | 任务超时 |
| `INTERNAL_ERROR` | 设备内部错误 |

`STALE_GENERATION` 是内部错误，不会作为协议错误码发送。
