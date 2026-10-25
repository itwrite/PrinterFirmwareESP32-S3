# MQTT 控制与 IJCH 任务分片协议

## 1. 状态和配置

MQTT 是可选承载，默认固件关闭：

```text
services.mqtt = false
```

启用时必须同时提供：

- `mqtt_uri`：Broker URI，例如 `mqtt://192.168.1.20:1883`。
- `mqtt_base_topic`：topic 根路径，例如 `printers`。
- `device_id`：固件生成，例如 `IJPD-ABC123`。

MQTT 使用 Broker URI 决定主机、端口和传输方式；固件不固定 MQTT TCP 端口。

## 2. Topics

假设：

```text
base_topic = printers
job_id     = job-001
```

固定 topic：

```text
printers/IJPD-ABC123/command
printers/IJPD-ABC123/event
printers/IJPD-ABC123/job/job-001/chunk
```

设备连接 Broker 后：

- 订阅 `<base>/<device_id>/command`，QoS 1。
- 订阅 `<base>/<device_id>/job/+/chunk`，QoS 1。
- 发布 `<base>/<device_id>/event`，QoS 1。

没有 retained payload、去重、续传或重传协议。

## 3. MQTT 控制消息

command payload 是与 TCP/BLE 相同的 IJPD JSON 请求正文：

```json
{"msg_id":"m-001","cmd":"get_device_info","data":{}}
```

payload 可带或不带末尾 `#`；设备在进入共享 ControlPlane 前自动补齐缺失的
`#`。响应和事件发布到 event topic，内容仍是完整的：

```text
JSON response + '#'
```

命令、字段、错误码见 [IJPD 控制协议](ijpd-control.md)。

注意：MQTT 回调线程不直接执行路由，消息先进入有界队列，再由应用 tick 处理。
command 队列深度为 `2`。MQTT 分片消息的有效控制正文上限约为 `2048` 字节；
队列满或超限消息可能被丢弃且没有单独的协议错误响应，发布端应限制速率。

## 4. MQTT 打印任务

控制请求必须使用：

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
    "transport": "mqtt",
    "bytes_per_row": 48,
    "total_rows": 100
  }
}#
```

成功结果为：

```json
{
  "job_id": "job-001",
  "status": "waiting_mqtt_chunks",
  "mqtt_chunk_topic": "printers/IJPD-ABC123/job/job-001/chunk",
  "data_token": 305419896,
  "nominal_packet_bytes": 480
}
```

之后发布 IJCH 二进制 payload。每个 IJCH payload 内部承载的是普通 IJPD 数据
字节流，因此整个 Hello 和数据帧可以跨越多个 MQTT message。

## 5. IJCH Envelope

所有整数 little-endian：

```text
offset  size  field
0       4     magic = "IJCH"
4       1     version = 1
5       1     flags = 0
6       1     job_id_len = 1..40
7       1     reserved = 0
8       4     chunk_index u32 LE
12      4     total_chunks u32 LE
16      4     payload_len u32 LE
20      4     payload_crc32 u32 LE
24      N     job_id bytes
24+N    M     payload bytes
```

约束：

- `job_id` 仅允许 `[A-Za-z0-9_-]`。
- `chunk_index` 从 `0` 开始，必须严格连续。
- `total_chunks > 0`，且 `chunk_index < total_chunks`。
- `payload_len <= 65535`。
- 完整 Envelope 最大约 `65599` 字节。
- `payload_crc32` 只覆盖 payload，不覆盖 header 或 job ID。
- Job ID 必须与 `start_print` 中的 `job_id` 相同。

CRC32 使用 CRC-32/ISO-HDLC，与 IJPD 数据协议相同。无效 magic、版本、保留
字段或长度返回 `INVALID_ARGUMENT`/`PACKET_LENGTH_INVALID`；CRC 错误返回
`PACKET_CRC_MISMATCH`；顺序错误返回 `PACKET_OUT_OF_ORDER`，并终止任务。

Python 构造示例：

```python
import struct
import zlib

job = b"job-001"
payload = hello_and_ijpd_frames
header = (
    b"IJCH" + bytes([1, 0, len(job), 0]) +
    struct.pack("<III", index, total_chunks, len(payload)) +
    struct.pack("<I", zlib.crc32(payload) & 0xffffffff)
)
mqtt.publish(topic, header + job + payload, qos=1)
```

## 6. MQTT 和 TCP 的选择

| 场景 | 推荐 |
|---|---|
| 局域网低延迟大图打印 | TCP `9100` + TCP `9101` |
| 手机近场控制 | BLE 控制 + Wi-Fi TCP 数据 |
| 云端命令和数据承载 | MQTT command/event + IJCH chunk |
| 原始热敏 ESC/POS | 可选 RAW TCP |

MQTT 任务没有断点续传。QoS 1 可能带来 Broker 层的重复投递，但固件要求
IJCH 严格递增、无法处理重复 chunk，因此发布端应保证单次、有序发送。

## 7. 当前实现注意事项

- 默认固件没有启动 MQTT client，连接 topic 不会得到响应。
- MQTT 任务应在服务已成功启动且 `capabilities.services.mqtt=true` 时使用。
- 当前实现只对 Job ID 和 chunk 顺序做校验，不提供业务层幂等键。
- 断开 Broker 会使活动 MQTT 任务失败，并通过可用控制通道报告
  `DATA_CONNECTION_LOST`。
