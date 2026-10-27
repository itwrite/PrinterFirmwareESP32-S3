# BLE GATT 控制协议

## 1. 角色和广播

设备作为 BLE Peripheral 使用 NimBLE 广播。默认设备名：

```text
IJPD-Printer
```

主广播包包含 128-bit 服务 UUID；完整设备名放在 scan response 中，以避免
legacy BLE 广播包的 31-byte 长度限制。客户端可以扫描服务 UUID 或主动读取
scan response 中的设备名后连接。

## 2. UUID

| 对象 | UUID |
|---|---|
| Service | `5a8f0001-7c2b-4d6e-9a01-3f5e8d7c2b01` |
| Write characteristic | `5a8f0002-7c2b-4d6e-9a01-3f5e8d7c2b01` |
| Notify characteristic | `5a8f0003-7c2b-4d6e-9a01-3f5e8d7c2b01` |
| Data Write characteristic | `5a8f0004-7c2b-4d6e-9a01-3f5e8d7c2b01` |
| Data ACK characteristic | `5a8f0005-7c2b-4d6e-9a01-3f5e8d7c2b01` |

Write characteristic 支持 `Write` 和 `Write Without Response`。Notify
characteristic 支持 `Notify`。

## 3. 数据方向

### App -> printer

向 Write characteristic 写入 IJPD 控制协议的字节分片。分片拼接后必须形成：

```text
JSON body + '#'
```

BLE 不复用 TCP 9101 socket，但支持一个 Phase 1 小任务数据路径。客户端先通过
控制特征发送 `start_print`，设置 `transport:"ble"`，再通过 Data Write 特征发送
IJPD Hello 和二进制帧；设备通过 Data ACK 特征确认 Hello 和完整 packet。

Phase 1 限制：单连接、单任务、最大 `512 KiB`、logical packet `1024 bytes`、
`compression:"none"`、`integrity:"crc32"`；断开后不支持续传。

### Printer -> App

先对 Notify characteristic 启用通知，再接收响应/事件分片。设备按当前协商
ATT MTU 的 `MTU - 3` 字节分片发送，每个完整 JSON 帧不会与另一个发送帧交错。

目标 MTU 是 `247`，目标通知分片为 `244` 字节；如果中心设备没有发起 MTU
交换，客户端必须按实际 MTU 处理，最小通知分片为 `20` 字节。

## 4. 连接和队列限制

- BLE 写入单个分片大于 `244` 字节会被拒绝。
- 设备接收队列深度为 `8`；队列满时该写入可能被丢弃。
- 客户端必须等待/控制写入速率，并在应用层保留完整帧重组状态。
- 控制半帧超时为 `3000 ms`。
- 没有连接或未订阅 Notify 时，设备不会缓存响应/事件。
- BLE 连接断开后应重新发现服务并重新订阅 Notify。
- 当前实现只保存一个连接句柄和一个通知目标；第三方应按单个活动中心设备
  对接，不要依赖多中心并行控制。
- Data ACK 必须启用 Notify；数据发送端应逐 packet 等待 ACK，避免压满 BLE host 队列。

## 5. 典型流程

1. 扫描 `IJPD-Printer` 或服务 UUID。
2. 连接设备。
3. 发现 Service 和两个 characteristic。
4. 对 Notify characteristic 启用 Notify。
5. 将 `get_device_info` JSON 按 MTU-3 分片写入 Write characteristic。
6. 拼接 Notify 数据，按 `#` 取出完整响应。
7. 根据 `capabilities` 选择后续控制和数据路径。

## 6. 安全边界

当前 GATT characteristic 未强制要求加密、认证或配对。BLE 控制通道不要用于
发送需要保密的应用数据；固件协议本身没有应用层认证、签名或加密。

BLE 只有在 `start_print.transport:"ble"` 时承载 Phase 1 小任务数据；普通控制面
仍使用 Control Write/Notify。大任务仍应通过 Wi-Fi TCP `9101` 或可选 MQTT 传输。
