# 喷墨打印机控制与数据协议规范 V1.0（Phase 0 定稿）

> 状态：**定稿（等待用户确认）**
> 日期：2026-10-15
> 适用固件：ESP32-S3 DIY 6 英寸彩色喷墨照片打印机
> 本文档是《ESP32-S3_喷墨打印机固件开发AI提词_V1.0.md》第五、六、八、十章的工程定稿，
> 与之冲突时以本文档为准；任何修改必须先走版本变更流程（见 §9）。

---

## 1. 总览

| 项 | 值 |
|---|---|
| 协议版本 | `1.0`（`get_device_info.protocol_version` 上报） |
| 控制通道 A | BLE GATT（NimBLE 外设角色，1 个写特征 + 1 个通知特征） |
| 控制通道 B | Wi-Fi TCP，端口 **9100**（`IJPD_CONTROL_TCP_PORT`） |
| 数据通道 | Wi-Fi TCP，端口 **9101**（`IJPD_DATA_TCP_PORT`），仅 1 个活动连接 |
| 控制报文编码 | UTF-8 JSON，以 `#`（0x23）作为帧结束符 |
| 二进制字节序 | **Little Endian**（所有多字节整数） |
| 完整性校验 | CRC-32/ISO-HDLC（即 zlib `crc32`，见 §6.4） |
| 压缩 | 无 / LZ4 Block（独立块 + 存储兜底规则，见 §6.5） |

设计原则：网络输入一律视为不可信；所有长度先校验后使用；任一错误不得导致越界、死锁或无限分配。

---

## 2. 控制通道

### 2.1 TCP 控制（端口 9100）

- 字节流，无消息边界；解析器必须支持：一条报文分多次到达、一次到达含多条报文。
- 同一时刻最多 **2** 个 TCP 控制连接（`IJPD_MAX_CTRL_TCP_CONNS`）；事件广播到所有已连接控制端（BLE + TCP）。
- 响应只发往发起命令的那条连接。

### 2.2 BLE GATT

固定 UUID（V1 常量，写入固件与 APP）：

| 用途 | UUID |
|---|---|
| 服务 | `5a8f0001-7c2b-4d6e-9a01-3f5e8d7c2b01` |
| 命令写入特征（Write + Write Without Response） | `5a8f0002-7c2b-4d6e-9a01-3f5e8d7c2b01` |
| 响应/事件通知特征（Notify） | `5a8f0003-7c2b-4d6e-9a01-3f5e8d7c2b01` |

- 单连接（外设角色，同一时刻仅接受 1 个中心设备）。
- MTU 协商目标 247；实际分片大小 = `MTU − 3`（下限 20）。
- 下行（APP→设备）：一条 JSON 可能被拆成多个 BLE 写分片，设备按 `#` 重组。
- 上行（设备→APP）：Notify 同样按 `MTU − 3` 分片发送，APP 按 `#` 重组。
- 重组缓冲区：每连接独立，容量 = `IJPD_MAX_CONTROL_FRAME_SIZE`（2048）。
- 重组超时：部分帧在 **3000 ms** 内没有新分片到达 → 丢弃半帧，通过 Notify 回 `FRAME_TIMEOUT` 错误（`msg_id: null`）。

### 2.3 帧规则（控制通道统一）

1. 帧结束符为第一个出现的字节 `#`（0x23）。**结束符之前的内容即 JSON 正文**。
2. 因此 JSON 正文中（含字符串、键名内部）**禁止出现裸 `#`**；发送方必须写成转义 `\u0023`。设备端写出 JSON 时执行同样转义。**此为协议强制约束，双方实现与测试用例必须覆盖。**
3. `MAX_CONTROL_FRAME_SIZE = 2048` 字节（JSON 正文，不含 `#`）。超限：丢弃数据直到下一个 `#`，回 `FRAME_TOO_LARGE`（`msg_id: null`），缓冲区不得增长。
4. 空帧（`#` 之前无任何字节）直接忽略，不回包。
5. 帧超时：部分帧 **3000 ms** 无新字节 → 丢弃半帧，回 `FRAME_TIMEOUT`（`msg_id: null`；无法提取 `msg_id` 时一律为 `null`）。
6. BLE 与 TCP 控制共用同一套 `frame_splitter → json_lite → command_router → 响应` 逻辑。

---

## 3. JSON 报文格式

### 3.1 请求

```json
{
  "msg_id": "unique-message-id",
  "cmd": "get_device_info",
  "data": {}
}#
```

| 字段 | 类型 | 规则 |
|---|---|---|
| `msg_id` | string | 1–40 字节（解码后）；响应原样带回；设备事件用 `evt-<十进制序号>`（如 `evt-0007`） |
| `cmd` | string | 非空，必须在命令白名单（§4.1）内；缺失/类型错误 → `INVALID_ARGUMENT`；字符串但不在白名单 → `UNKNOWN_COMMAND` |
| `data` | object | 必须存在且为 object；所有业务参数只能放在此字段内 |

顶层只允许这 3 个键；顶层出现其他键 → `INVALID_ARGUMENT`（严格模式）。

### 3.2 响应（成功与失败同构）

```json
{
  "msg_id": "unique-message-id",
  "cmd": "get_device_info",
  "data": {
    "success": true,
    "result": {},
    "error": null
  }
}#
```

失败示例：

```json
{
  "msg_id": "abc",
  "cmd": "start_print",
  "data": {
    "success": false,
    "result": null,
    "error": { "code": "INVALID_ARGUMENT", "message": "total_packets must be greater than zero" }
  }
}#
```

- `error` 非 null 时形如 `{ "code": "<CODE>", "message": "<固定英文文案>" }`。
- 解析失败无法提取 `msg_id` 时，响应 `msg_id` 为 `null`。
- 写出器：流式写入定长缓冲，每次写入检查容量；溢出整条失败并改发 `INTERNAL_ERROR` 短响应，**绝不输出被截断的 JSON**；自动转义 `"`、`\`、控制字符与 `#`（→ `\u0023`）。

### 3.3 错误码表（V1 全集，23 条，固件内提供唯一"码↔字符串"映射）

| code | 含义 |
|---|---|
| `INVALID_JSON` | JSON 语法/约束违反（深度、字符串长度、非法转义、非法 UTF-8 等） |
| `INVALID_ARGUMENT` | 参数类型/范围/组合非法，或未知字段 |
| `UNKNOWN_COMMAND` | cmd 不在白名单 |
| `FRAME_TOO_LARGE` | 控制帧超过 2048 字节 |
| `FRAME_TIMEOUT` | 控制帧重组超时 |
| `PACKET_OUT_OF_ORDER` | 分包序号重复/乱序/缺失（V1 严格顺序模式） |
| `PACKET_LENGTH_INVALID` | 分包长度字段非法或累计字节数不符 |
| `PACKET_CRC_MISMATCH` | CRC32 校验失败 |
| `DECOMPRESSION_FAILED` | LZ4 流损坏（输入越界、offset 非法、序列边界错误） |
| `DECOMPRESSED_LENGTH_MISMATCH` | 解压输出长度 ≠ expected_len |
| `QUEUE_TIMEOUT` | 等待槽位/缓冲超时 |
| `PSRAM_UNAVAILABLE` | PSRAM 未初始化或容量不足 |
| `OUT_OF_MEMORY` | 内存分配失败 |
| `JOB_ALREADY_RUNNING` | 已有活动任务 |
| `NO_ACTIVE_JOB` | 无活动任务时执行了需要任务的命令 |
| `DATA_CONNECTION_LOST` | 数据连接意外断开 |
| `DATA_HANDSHAKE_FAILED` | 数据连接握手失败（魔数/版本/令牌/保留字段） |
| `HAL_NOT_CONFIGURED` | HAL 未配置（真实硬件未接入） |
| `HAL_FAULT` | HAL 故障（含注入故障） |
| `UNSUPPORTED` | 设备不支持该能力（如当前固件的切纸） |
| `JOB_CANCELLED` | 任务被取消 |
| `JOB_TIMEOUT` | 任务超时（数据连接超时/包间空闲超时/总超时） |
| `INTERNAL_ERROR` | 内部错误（含响应序列化溢出） |

### 3.4 `json_lite` 子集约束（入站解析）

- 就地分词，token 数组固定容量 **64**，零堆分配。
- 类型：object、array、string、integer、boolean、null。**不支持浮点**：遇小数点或指数 → `INVALID_JSON`。
- 嵌套深度 ≤ **4**（入站请求最深为 `{root{data{...}}}` = 3 层，留 1 层余量）。
- 单个字符串解码后 ≤ **128** 字节；整数按 int64 解析并检查溢出。
- 同一对象内重复键 → `INVALID_JSON`。
- 字符串转义支持 `\" \\ \/ \b \f \n \r \t \uXXXX`；孤立代理项、非法转义、未转义控制字符（< 0x20）、非法 UTF-8 序列一律拒绝。
- **未知字段拒绝**（严格模式，含顶层）→ `INVALID_ARGUMENT`。
- 字段读取接口类型安全：类型不符返回错误而非默认值。
- 深度/长度限制只约束入站；出站大响应（如 `get_device_info`）由流式写出器分块输出，不受 64 token 限制。

---

## 4. 命令集

### 4.1 命令白名单（V1）

| cmd | 说明 | 硬件依赖 | `supported` 元数据 |
|---|---|---|---|
| `get_device_info` | 设备/能力/限制/命令元数据 | 无 | 恒 true |
| `start_print` | 创建打印任务 | PSRAM | PSRAM 可用时 true |
| `cancel_print` | 取消当前任务 | 无 | 恒 true |
| `get_print_status` | 查询当前/最近任务与统计 | 无 | 恒 true |
| `feed_paper` | 走纸指定距离 | 走纸 HAL | Mock 下 true（模拟走纸）；真实未接入前不报告可执行真实电机 |
| `cut_paper` | 切纸 | 切纸 HAL | **默认 false → `UNSUPPORTED`** |

命令元数据（label/description/params，含类型、min/max、必填、单位）由 `command_schema` 单一数据源生成，同时驱动参数校验与 `get_device_info` 输出，结构上保证"上报的命令 = 可路由的命令"。`get_diagnostics` 为 V1.1 候选，**不在本版白名单**。

### 4.2 `get_device_info`

无参数。`data.result` 结构：

| 键 | 内容 |
|---|---|
| `device` | `device_id`、`model`、`fw_version`、`protocol_version`("1.0")、`connections`(["ble","wifi_tcp"])、`hal_mode`("mock"\|"real") |
| `printhead` | `brand`/`model`/`nozzle_count`/`dpi`/`print_width_mm`/`interface`；未知一律 `null` + `is_configured:false`，**禁止编造** |
| `memory` | `psram_available`(bool)、`psram_total_bytes`、`psram_free_bytes`（仅在可可靠读取时上报，否则 null） |
| `capabilities` | `color`(bool)、`compressions`(["none","lz4_block"])、`integrity`(["none","crc32"])、`paper_feed`(bool)、`cutter`(bool)、`mock_print`(bool)、`crc32`(true) |
| `limits` | `max_control_frame`=2048、`max_packet_compressed`=65535、`max_packet_uncompressed`=65535、`max_job_bytes`=67108864、`max_total_packets`=65535、`queue_depth`=6、`control_port`=9100、`data_port`=9101 |
| `commands` | 命令元数据数组（含 `supported` 标志） |

### 4.3 `start_print`

请求参数：

| key | type | required | 约束 |
|---|---|---|---|
| `job_id` | string | 是 | 1–40 字节，仅 `[A-Za-z0-9_-]` |
| `total_bytes_uncompressed` | integer | 是 | 1 .. 67108864（64 MiB） |
| `total_packets` | integer | 是 | 1 .. 65535（16 位序号上限） |
| `compression` | enum | 是 | `none` \| `lz4_block` |
| `integrity` | enum | 否（默认 `none`） | `none` \| `crc32` |
| `bytes_per_row` | integer | 是 | 1 .. 65535 |
| `total_rows` | integer | 是 | 1 .. `total_bytes_uncompressed` |
| `cutter_after_finish` | boolean | 否（默认 false） | 设备不支持切纸时按警告处理，不影响打印结果 |

**派生量（设备计算并在响应回显，双方据此校验每包长度）：**

```text
nominal_packet_bytes = ceil(total_bytes_uncompressed / total_packets)
last_packet_bytes    = total_bytes_uncompressed − nominal_packet_bytes × (total_packets − 1)
```

校验顺序（全部通过才创建任务）：

1. 类型检查（含无未知字段、无浮点）。
2. 数值范围检查（见上表）。
3. 交叉检查（全部用无溢出乘法 `__builtin_mul_overflow` 等价实现）：
   - `bytes_per_row × total_rows == total_bytes_uncompressed`（严格相等，V1 无行尾填充）；
   - `total_packets ≤ total_bytes_uncompressed`（每包至少 1 字节）；
   - `total_bytes_uncompressed ≤ total_packets × 65535`；
   - `last_packet_bytes ∈ [1, nominal_packet_bytes]`（由上面两式自动满足，仍显式断言）。
4. 系统状态检查：PSRAM 可用、无活动任务（否则 `JOB_ALREADY_RUNNING`）、HAL 已初始化。

前置约束：**任务创建成功之前不接受任何数据包**；同一时间仅允许 1 个活动任务。

成功响应 `data.result`：

```json
{
  "job_id": "job-001",
  "status": "waiting_data_conn",
  "data_port": 9101,
  "data_token": 305419896,
  "nominal_packet_bytes": 4000,
  "compression": "lz4_block",
  "integrity": "crc32",
  "data_connect_timeout_ms": 10000,
  "limits": { "max_packet_uncompressed": 65535, "max_job_bytes": 67108864 }
}
```

- `data_token`：uint32，由硬件 RNG（`esp_fill_random`）生成，**一次性**、任务绑定；超时未用即作废。
- 在 `data_connect_timeout_ms` 内无合法数据连接握手 → 任务以 `JOB_TIMEOUT` 终止。

### 4.4 `cancel_print`

参数：`job_id`（可选，缺省取消当前任务）。无活动任务 → `NO_ACTIVE_JOB`。
执行：置取消标志 → 递增任务代次 → 唤醒各等待点 → HAL `stop()` → 回收槽位 → 关闭数据连接 → 终态 `CANCELLED`（响应 result 含 `job_id` 与终态）。

### 4.5 `get_print_status`

无参数。`data.result`：

```json
{
  "job_id": "job-001",
  "status": "receiving",
  "generation": 3,
  "stats": {
    "packets_received": 12, "packets_decoded": 10,
    "bytes_decoded": 40000, "bytes_printed": 32000,
    "queue_watermark": 2, "queue_watermark_max": 5,
    "throughput_bps": 486000,
    "crc_errors": 0, "decode_errors": 0, "elapsed_ms": 1023
  }
}
```

无任何历史任务时 `job_id: null`。终态（`completed/done_with_warning/cancelled/error`）保持到下一次 `start_print` 前可查。

### 4.6 `feed_paper` / `cut_paper`

- `feed_paper`：`{ "distance_um": 1..254000 }`；打印进行中 → `JOB_ALREADY_RUNNING`；走纸 HAL 未配置真实硬件 → Mock 模拟执行并返回 `{ "moved_um": N }`。
- `cut_paper`：无参数；V1 默认 `UNSUPPORTED`，元数据 `supported:false`；绝不允许在未确认硬件时驱动任何 GPIO。

---

## 5. 设备事件

事件由设备生成 `evt-<序号>`，经独立事件队列 + 序号发送，**广播到所有已连接控制端**；发送失败仅记录日志，不得影响物理打印流程。

### 5.1 `print_progress`（周期：每 200 ms 或每 10 包，先到者）

```json
{ "msg_id": "evt-0007", "cmd": "print_progress", "data": { "success": true,
  "result": { "job_id": "job-001", "status": "receiving",
    "packets_received": 12, "packets_decoded": 10, "bytes_decoded": 40000,
    "queue_watermark": 2, "queue_watermark_max": 5, "throughput_bps": 486000 },
  "error": null } }#
```

### 5.2 `print_finished`（唯一终态事件，覆盖 4 种终态）

- `status` ∈ `completed | done_with_warning | cancelled | error`。
- 正常：`success:true`，`result` 含 `packets_received/packets_decoded/bytes_printed/duration_ms/warning`。
- 异常：`success:false`，`error.code` 为对应错误码（如 `DATA_CONNECTION_LOST`）。

---

## 6. 数据通道二进制协议

### 6.1 连接与握手

- 任务活动期间仅允许 **1** 个数据连接；新连接在握手通过前不接收任何帧。
- 连接建立后客户端必须**先发 12 字节 Hello（仅一次，不属于分包帧）**：

```text
偏移  长度  字段          值
0     4    Magic         "IJPD"（0x49 0x4A 0x50 0x44）
4     2    Version       0x0001（LE）
6     4    Token         start_print 返回的 data_token（LE）
10    2    Reserved      0x0000（必须为 0，收到非 0 即失败）
```

设备校验：魔数、版本、令牌与当前任务一致**且未被使用过**、保留字段为 0。
失败：立即断开，任务 → `ERROR`，控制通道上报 `DATA_HANDSHAKE_FAILED`。
此机制将数据连接与任务严格绑定，防止数据注入与旧令牌重放。

### 6.2 分包帧格式

```text
+-------------------+----------------------+---------------------+--------------------+------------------+
| Packet Index (2B) | Uncompressed Len(2B) | Compressed Len (2B) | Payload            | CRC32 (4B)       |
|                   |                      |                     | Compressed Len 字节 | 仅 integrity=     |
|                   |                      |                     |                    | crc32 时存在      |
+-------------------+----------------------+---------------------+--------------------+------------------+
```

- 全部字段 **Little Endian**；固件内只允许 `read_u16_le/read_u32_le` 逐字节读取，禁止指针强转。
- 解析状态机：`HEADER(6B) → PAYLOAD(clen) → CRC(4B,可选) → DISPATCH`，逐字节/逐块累积，**不假设一次 read 等于一个完整帧**（半包、粘包、随机切分都必须正确）。

### 6.3 每包校验规则（HEADER 阶段即可判定的先判定）

| # | 规则 | 违反时错误码 |
|---|---|---|
| 1 | `Packet Index == 当前期望序号`（严格顺序：重复、乱序、缺包一律终止） | `PACKET_OUT_OF_ORDER` |
| 2 | `Uncompressed Len ∈ [1, 65535]` | `PACKET_LENGTH_INVALID` |
| 3 | `lz4_block` 模式：`Compressed Len ≤ Uncompressed Len`；`none` 模式：`Compressed Len == Uncompressed Len` | `PACKET_LENGTH_INVALID` |
| 4 | 非最后包：`Uncompressed Len == nominal_packet_bytes`；最后包：`Uncompressed Len == last_packet_bytes` | `PACKET_LENGTH_INVALID` |
| 5 | `CRC32(6B 头 + Payload) == 收到的 CRC`（仅 `integrity=crc32`） | `PACKET_CRC_MISMATCH` |
| 6 | 全部包的 `Uncompressed Len` 累计 == `total_bytes_uncompressed` | `PACKET_LENGTH_INVALID` |

任一违反：终止任务 → 终态 `ERROR` → 断开数据连接 → 控制通道上报对应错误码。**不做静默丢弃，不做 V1 重传**（ACK/NACK 留待后续版本，禁止静默加字段）。

### 6.4 CRC32

- 算法：CRC-32/ISO-HDLC（zlib/PKZIP 一致）：多项式 0x04C11DB7（反射 0xEDB88320）、初值 0xFFFFFFFF、输入/输出反射、终异或 0xFFFFFFFF。
- 标准测试向量：`"123456789"` → `0xCBF43926`。
- 覆盖范围：6 字节帧头 + Payload，不含 CRC 字段自身。

### 6.5 压缩与"存储兜底"规则（消除 16 位长度歧义）

- `compression = none`：载荷即像素数据，`clen == ulen` 强制。
- `compression = lz4_block`：每包为**独立 LZ4 Block**（无字典延续、非 LZ4 Frame）。
  - 编码端：若 LZ4 输出长度 ≥ 原始长度，改发原始数据（此时 `clen == ulen`）。
  - 解码端语义（**强制，消除歧义**）：
    - `clen == ulen` → 原始数据，直接入槽，**不解压**；
    - `clen < ulen` → LZ4 Block，解压后长度必须严格等于 `ulen`；
    - `clen > ulen` → 非法（§6.3 规则 3）。

### 6.6 数据通道错误处理表

| 情形 | 错误码 | 数据连接 | 任务终态 |
|---|---|---|---|
| Hello 魔数/版本/保留字段错 | `DATA_HANDSHAKE_FAILED` | 立即断开 | `ERROR` |
| 令牌不匹配/已使用/无活动任务 | `DATA_HANDSHAKE_FAILED` | 立即断开 | `ERROR`（若有任务） |
| 任务等待握手超时 | `JOB_TIMEOUT` | — | `ERROR` |
| 包间空闲 > 5000 ms | `JOB_TIMEOUT` | 断开 | `ERROR` |
| 任务总时长 > 600000 ms | `JOB_TIMEOUT` | 断开 | `ERROR` |
| 任一 §6.3 规则违反 | 对应错误码 | 断开 | `ERROR` |
| LZ4 损坏/输出长度不符 | `DECOMPRESSION_FAILED` / `DECOMPRESSED_LENGTH_MISMATCH` | 断开 | `ERROR` |
| 数据 TCP 中途断开 | `DATA_CONNECTION_LOST` | 已断 | `ERROR` |
| 槽位/压缩缓冲等待超时 | `QUEUE_TIMEOUT` | 断开 | `ERROR` |
| `cancel_print` | `JOB_CANCELLED` | 主动关闭 | `CANCELLED` |

---

## 7. 示例与测试向量

### 7.1 二进制帧示例（`none` + `crc32`，第 0 包，16 字节数据）

```text
Header : 00 00  10 00  10 00            (idx=0, ulen=16, clen=16)
Payload: 30 31 32 33 34 35 36 37 38 39 41 42 43 44 45 46   ("0123456789ABCDEF")
CRC32  : 3d 3a cb 12                    (crc32(header+payload)=0x12CB3A3D, LE)
```

### 7.2 CRC 标准向量

`crc32("123456789") = 0xCBF43926`。

### 7.3 `#` 转义示例

发送含 `#` 的 `job_id`（逻辑值 `job-#1`）：

```json
{ "msg_id": "m1", "cmd": "start_print", "data": { "job_id": "job-\u00231", ... } }#
```

注：V1 的 `job_id` 字符集为 `[A-Za-z0-9_-]`，本身不含 `#`；该示例用于验证转义通道（如 `msg_id`、后续版本字段）。

---

## 8. 默认参数（Phase 1 进入 `config_limits.h`，`static_assert` 自洽）

| 常量 | 值 | 说明 |
|---|---|---|
| `IJPD_CONTROL_TCP_PORT` | 9100 | |
| `IJPD_DATA_TCP_PORT` | 9101 | |
| `IJPD_MAX_CTRL_TCP_CONNS` | 2 | |
| `MAX_CONTROL_FRAME_SIZE` | 2048 | 入站 JSON 正文上限 |
| `MAX_RESPONSE_FRAME_SIZE` | 6144 | **出站响应帧上限**（APP 重组缓冲须 ≥ 此值；device_info 元数据较大） |
| `CTRL_FRAME_TIMEOUT_MS` | 3000 | 帧重组超时（BLE/TCP 通用） |
| `MAX_PACKET_UNCOMPRESSED_SIZE` | 65535 | 槽位分配 65536（对齐余量） |
| `MAX_PACKET_COMPRESSED_SIZE` | 65535 | |
| `MAX_JOB_BYTES` | 67108864 | 64 MiB |
| `MAX_TOTAL_PACKETS` | 65535 | |
| `SLOT_COUNT` / `SLOT_SIZE` | 6 / 65536 | PSRAM，启动期一次性分配 |
| `COMPRESSED_BUF_COUNT` | 2 × 65536 | PSRAM，rx↔decode 乒乓 |
| `HIGH_WATERMARK` / `LOW_WATERMARK` | 4 / 2 | 槽位（READY+PROCESSING 计数） |
| `DATA_CONNECT_TIMEOUT_MS` | 10000 | |
| `PACKET_IDLE_TIMEOUT_MS` | 5000 | |
| `JOB_TOTAL_TIMEOUT_MS` | 600000 | |
| `PROGRESS_INTERVAL_MS` / `PROGRESS_EVERY_PACKETS` | 200 / 10 | |
| `JSON_MAX_TOKENS` / `JSON_MAX_DEPTH` / `JSON_MAX_STR` | 64 / 4 / 128 | |
| `MSG_ID_MAX` / `JOB_ID_MAX` | 40 / 40 | |

---

## 9. 兼容性与版本演进规则

1. V1 帧内不新增任何字段；扩展（ACK/NACK、重传、浮点、新命令）必须提升协议版本并同步更新本文档、`command_schema` 与双方测试。
2. 所有保留字段必须为 0，接收端校验非 0 即拒绝，为未来扩展保底。
3. Hello `Version` 不匹配时直接拒绝连接（不做协商）。
4. 新命令进入白名单前必须先出现在 `command_schema` 元数据中（单一数据源）。
