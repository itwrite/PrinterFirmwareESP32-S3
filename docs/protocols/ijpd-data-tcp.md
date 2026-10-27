# IJPD TCP 二进制数据协议 V1

## 1. 连接流程

控制面和数据面是两个独立的 TCP 连接：

1. 在控制连接发送 `start_print`。
2. 读取响应中的 `data_token`、`data_port`、`nominal_packet_bytes`。
3. 连接打印机 TCP `9101`。
4. 发送 12 字节 IJPD Hello。
5. 按顺序发送二进制数据帧。
6. 保持控制连接，等待 `print_progress` 和 `print_finished`。

TCP `9101` 只在有活动任务且等待/接收数据时接受连接；没有任务时连接会被
立即关闭。每个任务只允许一个数据连接。连接在
`data_connect_timeout_ms`（当前默认 `10000 ms`）内完成 Hello。

## 2. Hello

所有整数均为 little-endian：

| 偏移 | 长度 | 字段 | 值 |
|---:|---:|---|---|
| 0 | 4 | Magic | ASCII `IJPD` |
| 4 | 2 | Version | `0x0001` |
| 6 | 4 | Token | `start_print` 返回的 `data_token` |
| 10 | 2 | Reserved | 必须为 `0x0000` |

C/Python 示例：

```python
import struct

hello = b"IJPD" + struct.pack("<HIH", 1, data_token, 0)
sock.sendall(hello)
```

magic、version、token 或 reserved 错误返回 `DATA_HANDSHAKE_FAILED`，并终止
任务。

## 3. 数据帧

```text
+------------------+----------------------+--------------------+------------------+----------------+
| index u16 LE     | ulen u16 LE          | clen u16 LE        | payload          | crc u32 LE     |
|                  |                      |                    | clen bytes       | optional       |
+------------------+----------------------+--------------------+------------------+----------------+
```

- `index`：从 `0` 开始，严格递增。
- `ulen`：解压后的长度。
- `clen`：payload 的实际长度。
- `payload`：原始数据或独立 LZ4 Block。
- `crc`：仅 `integrity:"crc32"` 时存在，覆盖前 6 字节 header 和 payload。

发送端可以任意切分 TCP write；接收端也可以一次收到多个帧。设备按字节流
解析，不假设一次 `send()` 对应一次 `recv()`。

## 4. 长度和压缩规则

### `compression:"none"`

```text
clen == ulen
payload == 原始打印数据
```

### `compression:"lz4_block"`

- 每帧都是独立的 LZ4 Block，不使用 LZ4 Frame，不跨帧使用字典。
- `clen == ulen`：存储兜底，payload 是原始数据，不解压。
- `clen < ulen`：payload 是 LZ4 Block，解压后必须严格得到 `ulen` 字节。
- `clen > ulen`：非法，返回 `PACKET_LENGTH_INVALID`。

### 包序列

```text
nominal = ceil(total_bytes_uncompressed / total_packets)
last = total_bytes_uncompressed - nominal * (total_packets - 1)
```

- 非最后一包：`ulen == nominal`。
- 最后一包：`ulen == last`。
- `index` 必须严格等于当前期望值。
- 全部包的 `ulen` 总和必须等于 `total_bytes_uncompressed`。
- 最后一个包完成后不允许还有多余字节。

## 5. CRC32

算法为 CRC-32/ISO-HDLC，也就是 zlib `crc32`：

```text
多项式: 0x04C11DB7（反射实现常量 0xEDB88320）
初值:   0xFFFFFFFF
终异或: 0xFFFFFFFF
```

标准向量：

```text
crc32("123456789") == 0xCBF43926
```

CRC 字段自身不参与计算。CRC 失败返回 `PACKET_CRC_MISMATCH`。

Python 构造帧示例：

```python
import struct
import zlib

header = struct.pack("<HHH", index, len(raw), len(raw))
frame_without_crc = header + raw
crc = struct.pack("<I", zlib.crc32(frame_without_crc) & 0xffffffff)
sock.sendall(frame_without_crc + crc)
```

## 6. 资源和超时限制

| 项目 | 当前值 |
|---|---:|
| 单包最大解压长度 | `65535` bytes |
| 单包最大压缩长度 | `65535` bytes |
| 单任务最大原始数据 | `67108864` bytes |
| 最大包数 | `65535` |
| 等待数据连接 | `10000 ms` |
| 包间空闲超时 | `15000 ms` |
| 任务总超时 | `600000 ms` |
| TCP 数据接收临时块 | `1024` bytes |
| 并发数据连接 | `1` |

普通流水线使用 PSRAM 槽位和 TCP 背压；当内部水位达到上限时，设备暂停读取
数据 socket，客户端应继续保持连接并等待窗口恢复，不要主动重连或重发已发送包。

## 7. 终态和断开

- 客户端主动关闭数据连接通常导致 `DATA_CONNECTION_LOST`，但在页缓存直通路径
  中，异常处理受当前固件实现影响，第三方应以控制面终态事件为准。
- 包校验、长度或 Hello 错误会终止任务；协议没有 ACK、NACK、重传或续传。
- `cancel_print` 的即时响应是 `cancelling`，真正终态稍后通过事件发送。
- 完成、错误、取消后，设备会关闭数据 socket；控制连接仍可继续查询状态。

## 8. 当前默认镜像限制

默认固件使用 `RICOH-MH5440-MOCK` Profile，报告 A4 logical `CMYK8` raster 能力：
`2480×3508 px`、`9920 bytes/row`、参考 `300 dpi`。这些是逻辑 Mock 契约，
不是已确认的真实喷头电气规格；客户端仍应读取 `get_device_info`，不要据此推断
真实波形、电压、连接器或喷嘴时序。

当前推荐路径是 `mode:"raster"` + TCP `9101`。A4 logical Mock 的完整一页约为
`2480×3508×4 = 34.8 MiB`，发送端应使用 TCP 路径和 LZ4/CRC32；BLE Phase 1
的 `512 KiB` 上限只适合小测试任务。`mode:"escpos"` 虽出现在
schema 中，但默认镜像不提供可用的 IJPD ESC/POS 流水线；原始 ESC/POS 请使用
独立 RAW TCP 协议。

## 9. BLE 备用数据传输

无 Wi-Fi 时可使用 IJPD BLE Service 的 Data Write/Data ACK 特征：

```json
{"cmd":"start_print","data":{"transport":"ble","compression":"none","integrity":"crc32"}}
```

Phase 1 限制：最大任务 `512 KiB`、logical packet `1024 bytes`、单连接单任务、
逐 packet ACK、断开后重新开始。当前实现要求 `compression:"none"` 且
`integrity:"crc32"`。BLE 复用本协议的 IJPD Hello、6-byte packet header、payload
和 CRC32，但不提供 TCP socket、断点续传或多物理链路并发。
