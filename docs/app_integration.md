# IJPD 打印机协议 App 对接文档

> **版本**：v1.1 · 2026-10-15
> **目标读者**：App 开发者（iOS/Android/PC）或 AI 编程助手
> **目的**：根据本文档实现与任何支持 IJPD 中间件的打印机（热敏/喷墨/标签）通信的客户端

---

## 一、快速理解

你的 App 通过 **Wi-Fi TCP** 或 **BLE** 连接打印机，用 **JSON 命令** 控制它，用 **二进制分包** 传输打印数据。

```
┌──────────────┐         ┌─────────────────────────────────┐
│              │  TCP 9100│  控制通道：JSON 命令 + 事件      │
│   Your App   │─────────▶│  (get_device_info / start_print │
│              │         │   / cancel / status / ...)      │
│              │         ├─────────────────────────────────┤
│              │  TCP 9101│  数据通道：二进制分包            │
│              │─────────▶│  (LZ4 压缩 + CRC32 校验)        │
└──────────────┘         └─────────────────────────────────┘
```

**一次完整打印流程**：
1. 连接 Wi-Fi → 获取设备 IP
2. TCP 连 9100（控制）→ 发 `get_device_info` → 获知打印机能力
3. 发 `start_print` → 获得 `data_token` + `data_port`
4. TCP 连 9101（数据）→ 发 12 字节 Hello（含 token）→ 发分包
5. 接收 `print_progress` / `print_finished` 事件

---

## 二、连接发现

### 2.1 Wi-Fi（推荐）

打印机以 STA 模式加入同一局域网。

| 项 | 值 |
|---|---|
| 控制端口 | **9100** (TCP) |
| 数据端口 | **9101** (TCP) |
| 并发控制连接 | 最多 2 个 TCP + 1 个 BLE |
| 并发数据连接 | 仅 1 个（任务期间独占） |

发现方式（任选）：
- mDNS / Bonjour（后续版本支持）
- 用户手动输入 IP
- 设备屏幕/串口显示 IP

### 2.2 BLE（备选）

| 项 | 值 |
|---|---|
| 设备名 | `IJPD-Printer` |
| 服务 UUID | `5a8f0001-7c2b-4d6e-9a01-3f5e8d7c2b01` |
| 写特征（发命令） | `5a8f0002-7c2b-4d6e-9a01-3f5e8d7c2b01` |
| 通知特征（收响应） | `5a8f0003-7c2b-4d6e-9a01-3f5e8d7c2b01` |

BLE 仅支持**控制通道**（打印数据量大，走 Wi-Fi）。分片规则见 §3.3。

---

## 三、控制协议（JSON + `#`）

### 3.1 请求格式

每条消息以 `#`（ASCII 0x23）结尾，正文为 UTF-8 JSON：

```json
{"msg_id":"m-001","cmd":"get_device_info","data":{}}#
```

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `msg_id` | string | 是 | 1–40 字节唯一标识；响应原样带回 |
| `cmd` | string | 是 | 命令名（见 §四） |
| `data` | object | 是 | 业务参数；无参数时传 `{}` |

### 3.2 响应格式

```json
{"msg_id":"m-001","cmd":"get_device_info","data":{"success":true,"result":{...},"error":null}}#
```

失败时：

```json
{"msg_id":"m-001","cmd":"start_print","data":{"success":false,"result":null,"error":{"code":"INVALID_ARGUMENT","message":"..."}}}#
```

### 3.3 ⚠️ 关键约束：`#` 转义

JSON 正文中**禁止出现裸 `#` 字符**。如需包含（如密码、自定义 ID），必须写成 `\u0023`。

**发送端**：构造 JSON 后，把所有 `#` 替换为 `\u0023`，再追加 `#` 作为帧结束。
**接收端**：按第一个 `#` 拆帧，JSON 解析自动还原 `\u0023` 为 `#`。

### 3.4 BLE 分片重组

| 方向 | 分片大小 | 规则 |
|---|---|---|
| App → 设备 | `MTU - 3`（通常 244） | 拆成多个 Write 分片，设备按 `#` 重组 |
| 设备 → App | `MTU - 3` | Notify 分片，App 按 `#` 重组 |

**超时**：3000ms 无新分片 → 设备丢弃半帧并回 `FRAME_TIMEOUT`。

### 3.5 帧上限

单帧 JSON 正文最大 **2048** 字节（不含 `#`）。超限 → `FRAME_TOO_LARGE`。

---

## 四、命令列表

### 4.1 get_device_info

查询设备能力。**App 必须首先调用此命令来适配不同打印机。**

```json
// 请求
{"msg_id":"m-001","cmd":"get_device_info","data":{}}#

// 响应（热敏示例）
{"msg_id":"m-001","cmd":"get_device_info","data":{"success":true,"result":{
  "device":{"device_id":"IJPD-XXXXXX","model":"TP-58","fw_version":"1.0.0","protocol_version":"1.1","connections":["ble","wifi_tcp"],"hal_mode":"mock"},
  "printhead":{"brand":null,"model":null,"nozzle_count":null,"dpi":203,"print_width_px":384,"pixel_format":"mono1","is_configured":false},
  "memory":{"psram_available":true,"psram_total_bytes":8388608,"psram_free_bytes":7000000},
  "capabilities":{
    "printer_type":"thermal",
    "color":false,
    "data_modes":["raster","escpos"],
    "compressions":["none","lz4_block"],
    "integrity":["none","crc32"],
    "paper_feed":true,
    "cutter":true,
    "mock_print":true,
    "crc32":true,
    "sensors":["paper_out","cover_open","overtemp"]
  },
  "limits":{
    "max_control_frame":2048,
    "max_packet_compressed":65535,
    "max_packet_uncompressed":65535,
    "max_job_bytes":67108864,
    "max_total_packets":65535,
    "queue_depth":6,
    "control_port":9100,
    "data_port":9101
  },
  "commands":[...命令元数据...]
},"error":null}}#
```

**App 适配关键字段**：

| 字段 | 用途 |
|---|---|
| `printer_type` | "thermal" / "inkjet" / "label" / "dot_matrix" → 选择 UI 模板 |
| `pixel_format` | "mono1"（热敏 1bit）/ "cmyk8"（喷墨 32bit）→ 图像光栅化策略 |
| `print_width_px` + `dpi` | 页面尺寸计算 |
| `data_modes` | ["raster","escpos"] → 支持哪些打印模式 |
| `cutter` | 是否有切刀 → 显示/隐藏切纸按钮 |
| `sensors` | 需要监听的传感器事件列表 |
| `color` | true=彩色，false=黑白 |

### 4.2 start_print

创建打印任务，获取数据通道令牌。

#### raster 模式（结构化栅格行）

```json
{"msg_id":"m-002","cmd":"start_print","data":{
  "job_id":"my-job-001",
  "total_bytes_uncompressed":4800,
  "total_packets":10,
  "compression":"lz4_block",
  "integrity":"crc32",
  "mode":"raster",
  "bytes_per_row":48,
  "total_rows":100,
  "cutter_after_finish":false
}}#
```

| 参数 | 类型 | 必填 | 约束 |
|---|---|---|---|
| `job_id` | string | 是 | 1–40 字节，仅 `[A-Za-z0-9_-]` |
| `total_bytes_uncompressed` | int | 是 | 1–67108864；必须 = `bytes_per_row × total_rows` |
| `total_packets` | int | 是 | 1–65535 |
| `compression` | enum | 是 | `"none"` 或 `"lz4_block"` |
| `integrity` | enum | 否 | `"none"`（默认）或 `"crc32"` |
| `mode` | enum | 否 | `"raster"`（默认）或 `"escpos"` |
| `bytes_per_row` | int | raster 时必填 | 1–65535；受 Profile 宽度约束 |
| `total_rows` | int | raster 时必填 | ≥1 |
| `cutter_after_finish` | bool | 否 | 默认 false |

**响应**：

```json
{"msg_id":"m-002","cmd":"start_print","data":{"success":true,"result":{
  "job_id":"my-job-001",
  "status":"waiting_data_conn",
  "data_port":9101,
  "data_token":305419896,
  "nominal_packet_bytes":480,
  "compression":"lz4_block",
  "integrity":"crc32",
  "mode":"raster",
  "data_connect_timeout_ms":10000,
  "limits":{"max_packet_uncompressed":65535,"max_job_bytes":67108864}
},"error":null}}#
```

| 响应字段 | 说明 |
|---|---|
| `data_token` | uint32 令牌，**一次性**，用于数据通道握手 |
| `nominal_packet_bytes` | 标称包长 = ceil(total_bytes / total_packets)；除最后一包外每包 `ulen` 必须等于此值 |
| `data_connect_timeout_ms` | 在此时间内必须完成数据连接握手，否则任务超时 |

#### escpos 模式（ESC/POS 原始流透传）

适用于热敏打印机兼容已有 ESC/POS 内容。**不需要** `bytes_per_row` / `total_rows`：

```json
{"msg_id":"m-003","cmd":"start_print","data":{
  "job_id":"receipt-001",
  "total_bytes_uncompressed":216,
  "total_packets":3,
  "compression":"none",
  "integrity":"crc32",
  "mode":"escpos"
}}#
```

设备将原始字节流按块转发给热敏头，App 不需要理解 ESC/POS 内容。

### 4.3 cancel_print

```json
{"msg_id":"m-004","cmd":"cancel_print","data":{}}#
// 或指定 job_id
{"msg_id":"m-004","cmd":"cancel_print","data":{"job_id":"my-job-001"}}#
```

可在任何任务状态下调用。响应后设备会广播 `print_finished`（status=cancelled）。

### 4.4 get_print_status

```json
{"msg_id":"m-005","cmd":"get_print_status","data":{}}#
```

```json
{"msg_id":"m-005","cmd":"get_print_status","data":{"success":true,"result":{
  "job_id":"my-job-001",
  "status":"receiving",
  "generation":3,
  "stats":{"packets_received":12,"packets_decoded":10,"bytes_decoded":4800,
           "bytes_printed":2400,"queue_watermark":2,"queue_watermark_max":5,
           "throughput_bps":48600,"crc_errors":0,"decode_errors":0}
},"error":null}}#
```

### 4.5 feed_paper

```json
{"msg_id":"m-006","cmd":"feed_paper","data":{"distance_um":10000}}#
```

`distance_um`：1–254000（微米）。仅在无活动任务时可用。

### 4.6 cut_paper

```json
{"msg_id":"m-007","cmd":"cut_paper","data":{}}#
```

仅在 `capabilities.cutter == true` 时可用，否则返回 `UNSUPPORTED`。

---

## 五、设备事件（设备 → App 主动推送）

事件以 `print_progress` 或 `print_finished` 命令广播到**所有已连接控制端**（TCP + BLE）。`msg_id` 格式为 `evt-NNNN`。

### 5.1 print_progress（周期 ~200ms 或每 10 包）

```json
{"msg_id":"evt-0007","cmd":"print_progress","data":{"success":true,"result":{
  "job_id":"my-job-001",
  "status":"receiving",
  "packets_received":12,
  "packets_decoded":10,
  "bytes_decoded":4800,
  "queue_watermark":2,
  "queue_watermark_max":5,
  "throughput_bps":48600
},"error":null}}#
```

### 5.2 print_finished（终态，每个任务恰好一次）

```json
{"msg_id":"evt-0008","cmd":"print_finished","data":{"success":true,"result":{
  "job_id":"my-job-001",
  "status":"completed",
  "packets_received":30,
  "packets_decoded":30,
  "bytes_printed":14400,
  "duration_ms":1234,
  "warning":null
},"error":null}}#
```

| `status` 值 | `success` | 含义 |
|---|---|---|
| `completed` | true | 打印成功 |
| `done_with_warning` | true | 成功但有警告（如要求切纸但不支持） |
| `cancelled` | false | 被取消 |
| `error` | false | 出错（`error.code` 指明原因） |

### 5.3 传感器事件（Phase M2 预留）

热敏打印机具备传感器时会推送：

```json
{"msg_id":"evt-0009","cmd":"sensor_alert","data":{"success":true,"result":{
  "sensor":"paper_out",
  "state":true
},"error":null}}#
```

---

## 六、数据协议（二进制分包）

### 6.1 数据通道握手

`start_print` 成功后，在 `data_connect_timeout_ms`（默认 10 秒）内连接 TCP 9101 并发送 **12 字节 Hello**：

```
偏移  长度  字段       值
0     4    Magic      "IJPD"（0x49 0x4A 0x50 0x44）
4     2    Version    0x0001（Little Endian）
6     4    Token      start_print 返回的 data_token（LE）
10    2    Reserved   0x0000（必须为 0）
```

握手失败 → 设备断开连接，任务进入 error 状态。

### 6.2 分包帧格式

```
+-------------------+----------------------+---------------------+--------------------+------------------+
| Packet Index (2B) | Uncompressed Len(2B) | Compressed Len (2B) | Payload            | CRC32 (4B)       |
|                   |                      |                     | Compressed Len 字节 | 仅 integrity=     |
|                   |                      |                     |                    | crc32 时存在      |
+-------------------+----------------------+---------------------+--------------------+------------------+
```

所有多字节整数为 **Little Endian**。

| 字段 | 大小 | 说明 |
|---|---|---|
| `Packet Index` | 2B | 从 0 开始，严格顺序递增（0, 1, 2, ..., total_packets-1） |
| `Uncompressed Len` | 2B | 解压后长度 |
| `Compressed Len` | 2B | 载荷实际长度 |
| `Payload` | 变长 | `Compressed Len` 字节的压缩/原始数据 |
| `CRC32` | 4B | 可选；多项式 0x04C11DB7（zlib `crc32`），覆盖 6B 帧头 + 载荷 |

### 6.3 包长度规则

设备在 `start_print` 响应中返回 `nominal_packet_bytes`：
- **非末包**：`Uncompressed Len` **必须等于** `nominal_packet_bytes`
- **末包**（index = total_packets - 1）：可以短，等于剩余字节数
- 所有包的 `Uncompressed Len` 之和 **必须等于** `total_bytes_uncompressed`

### 6.4 压缩规则

| compression | clen vs ulen | 含义 |
|---|---|---|
| `"none"` | `clen == ulen` | 载荷即原始数据 |
| `"lz4_block"` | `clen == ulen` | 存储兜底：载荷为原始数据（不压缩时） |
| `"lz4_block"` | `clen < ulen` | 载荷为独立 LZ4 Block |
| `"lz4_block"` | `clen > ulen` | **非法** |

**LZ4 编码建议**：使用标准 LZ4 Block 格式压缩；如果输出 ≥ 原始长度，直接发原始数据（`clen == ulen`）。

### 6.5 严格顺序

- 只接受预期序号。重复、乱序、缺包 → `PACKET_OUT_OF_ORDER`，任务终止
- App 侧应按顺序发送，不重传（V1 无 ACK/NACK）
- 进度依赖 `print_progress` 事件，不依赖 TCP ACK

---

## 七、错误码表

| code | 含义 | App 处理建议 |
|---|---|---|
| `INVALID_JSON` | JSON 语法错误 | 检查请求格式 |
| `INVALID_ARGUMENT` | 参数类型/范围/组合错误 | 检查参数（`message` 字段有详情） |
| `UNKNOWN_COMMAND` | 命令不在白名单 | 检查 `cmd` 拼写 |
| `FRAME_TOO_LARGE` | 控制帧 > 2048 字节 | 减小 JSON 体积 |
| `FRAME_TIMEOUT` | 帧重组超时（3 秒） | BLE 分片发送过慢 |
| `JOB_ALREADY_RUNNING` | 已有活动任务 | 先 `cancel_print` 或等完成 |
| `NO_ACTIVE_JOB` | 无活动任务 | 不需处理 |
| `DATA_CONNECTION_LOST` | 数据连接断开 | 检查网络；任务已终止 |
| `DATA_HANDSHAKE_FAILED` | Hello 握手失败 | 检查 token/版本/保留字段 |
| `PACKET_OUT_OF_ORDER` | 包序号错误 | 确保按顺序发送 |
| `PACKET_LENGTH_INVALID` | 包长不合规 | 检查 nominal/last 规则 |
| `PACKET_CRC_MISMATCH` | CRC 校验失败 | 检查 CRC 计算（zlib crc32） |
| `DECOMPRESSION_FAILED` | LZ4 解压失败 | 检查 LZ4 编码正确性 |
| `PSRAM_UNAVAILABLE` | 设备内存不足 | 无法打印，联系设备端 |
| `UNSUPPORTED` | 设备不支持该功能 | 检查 `get_device_info` capabilities |
| `JOB_TIMEOUT` | 任务超时 | 检查数据发送速度/网络 |
| `JOB_CANCELLED` | 任务被取消 | 正常流程 |
| `HAL_FAULT` | 硬件故障 | 检查打印机状态 |
| `INTERNAL_ERROR` | 设备内部错误 | 重试或重启设备 |

---

## 八、完整示例代码（Python，可直接运行）

### 8.1 最简打印（raster 模式）

```python
import socket, json, struct, zlib, random

HOST = "192.168.1.100"  # 替换为设备 IP

def send_cmd(sock, msg_id, cmd, data):
    frame = json.dumps({"msg_id": msg_id, "cmd": cmd, "data": data},
                       separators=(",", ":")).replace("#", "\\u0023") + "#"
    sock.sendall(frame.encode())
    buf = b""
    while b"#" not in buf:
        buf += sock.recv(4096)
    raw = buf.split(b"#")[0].decode()
    return json.loads(raw)

# 1. 查设备信息
ctrl = socket.create_connection((HOST, 9100), timeout=5)
info = send_cmd(ctrl, "q1", "get_device_info", {})
result = info["data"]["result"]
print(f"打印机类型: {result['capabilities']['printer_type']}")
print(f"像素格式: {result['printhead']['pixel_format']}")
print(f"宽度: {result['printhead']['print_width_px']}px @ {result['printhead']['dpi']}dpi")

# 2. 准备打印数据（示例：100 行 × 48 字节/行 = 4800 字节）
bpr = 48; rows = 100; total = bpr * rows
pixels = bytes(random.getrandbits(8) for _ in range(total))

# 3. 创建任务
resp = send_cmd(ctrl, "q2", "start_print", {
    "job_id": "test-001",
    "total_bytes_uncompressed": total,
    "total_packets": 10,
    "compression": "none",
    "integrity": "crc32",
    "mode": "raster",
    "bytes_per_row": bpr,
    "total_rows": rows,
})
assert resp["data"]["success"], resp["data"]["error"]
token = resp["data"]["result"]["data_token"]
nominal = resp["data"]["result"]["nominal_packet_bytes"]

# 4. 数据连接 + 握手
data = socket.create_connection((HOST, 9101), timeout=5)
hello = b"IJPD" + struct.pack("<HIH", 1, token, 0)
data.sendall(hello)

# 5. 发送分包（严格顺序）
packets = 10
for k in range(packets):
    ulen = nominal if k < packets - 1 else total - nominal * (packets - 1)
    payload = pixels[k * nominal : k * nominal + ulen]
    frame = struct.pack("<HHH", k, ulen, ulen) + payload
    crc = zlib.crc32(frame) & 0xFFFFFFFF
    data.sendall(frame + struct.pack("<I", crc))

# 6. 接收事件
import select
while True:
    r, _, _ = select.select([ctrl], [], [], 5)
    if r:
        buf = ctrl.recv(8192).decode()
        for line in buf.split("#"):
            if not line: continue
            ev = json.loads(line)
            if ev["cmd"] == "print_progress":
                s = ev["data"]["result"]
                print(f"进度: {s['packets_received']}/{packets} 包, {s['throughput_bps']}B/s")
            elif ev["cmd"] == "print_finished":
                print(f"完成: {ev['data']['result']['status']}")
                break
        else:
            continue
        break

data.close(); ctrl.close()
```

### 8.2 ESC/POS 透传（热敏收据）

```python
# 构造 ESC/POS 数据
escpos_data = b"\x1B\x40"                    # 初始化
escpos_data += b"\x1Ba\x01"                   # 居中
escpos_data += b"Hello World\n"               # 文本
escpos_data += b"\x1D\x56\x00"                # 切纸

# start_print 用 escpos 模式（不需要 bytes_per_row）
resp = send_cmd(ctrl, "q3", "start_print", {
    "job_id": "receipt-001",
    "total_bytes_uncompressed": len(escpos_data),
    "total_packets": 1,
    "compression": "none",
    "integrity": "crc32",
    "mode": "escpos",
})
token = resp["data"]["result"]["data_token"]

# 连数据通道发 Hello + 一整包
data = socket.create_connection((HOST, 9101))
data.sendall(b"IJPD" + struct.pack("<HIH", 1, token, 0))
frame = struct.pack("<HHH", 0, len(escpos_data), len(escpos_data)) + escpos_data
crc = zlib.crc32(frame) & 0xFFFFFFFF
data.sendall(frame + struct.pack("<I", crc))
```

### 8.3 BLE 连接（iOS/Android 伪代码）

```
1. 扫描设备名 "IJPD-Printer"
2. 连接 → 发现服务 5a8f0001-...
3. 订阅通知特征 5a8f0003-...
4. 协商 MTU 247
5. 发命令：把 JSON+# 按 (MTU-3) 字节拆片，依次写入特征 5a8f0002
6. 收响应：通知特征按字节拼接，遇到 # 拆帧解析 JSON
7. 超时：3 秒未收到 # → 丢弃缓冲
```

---

## 九、App 适配决策树

```
get_device_info
    │
    ├─ printer_type == "thermal"
    │   ├─ pixel_format == "mono1" → 黑白光栅化（1bit/px）
    │   ├─ data_modes 含 "escpos" → 可透传 ESC/POS
    │   └─ sensors 有 "paper_out" → 监听 sensor_alert，缺纸时提示
    │
    ├─ printer_type == "inkjet"
    │   ├─ pixel_format == "cmyk8" → CMYK 四通道光栅化
    │   ├─ data_modes 只有 "raster" → 不支持 ESC/POS
    │   └─ print_width_px = 1800, dpi = 300 → 6寸照片排版
    │
    ├─ printer_type == "label"
    │   └─ 类似热敏，可能更窄（如 384px = 48mm）
    │
    └─ 未知类型 → 显示"不支持的打印机"，建议升级固件
```

---

## 十、调试与测试

### 10.1 手动测试命令

```bash
# 用 Python 单行命令快速测试
python3 -c "
import socket, json
s = socket.create_connection(('192.168.1.100', 9100))
s.sendall(b'{\"msg_id\":\"t1\",\"cmd\":\"get_device_info\",\"data\":{}}#')
print(s.recv(8192).decode()[:500])
"
```

### 10.2 完整模拟客户端

```bash
# 参考实现（含 ESC/POS、取消、CRC 损坏等场景）
python3 tools/send_job.py --host <IP> --bytes 4800 --packets 10 --bpr 48
python3 tools/send_job.py --host <IP> --cancel-after 5
python3 tools/send_job.py --host <IP> --corrupt-crc
```

### 10.3 常见问题

| 问题 | 排查 |
|---|---|
| 连接 9100 超时 | 确认设备已连 Wi-Fi，IP 正确，防火墙未拦 |
| `FRAME_TOO_LARGE` | JSON 超过 2048 字节；缩减 `data` 内参数 |
| `DATA_HANDSHAKE_FAILED` | token 错误/已使用；Hello 保留字段非 0 |
| `PACKET_OUT_OF_ORDER` | 未按 0,1,2... 顺序发送 |
| `PACKET_LENGTH_INVALID` | 非末包 ulen ≠ nominal；或 ulen 之和 ≠ total |
| `PACKET_CRC_MISMATCH` | CRC 算法不一致；确认用 zlib.crc32（不是 CRC32C） |
| `JOB_TIMEOUT` | 10 秒内未连接数据通道；或包间超 5 秒 |
| 收不到事件 | 事件通过控制通道（9100/BLE）推送，确认保持连接 |
