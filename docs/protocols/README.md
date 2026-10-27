# IJPD Third-Party Protocols

本文档目录描述当前默认 ESP32-S3 固件实际实现的外部协议。文档以源码和默认
`main/app_main.cpp` 配置为准；AirPrint 是独立的 opt-in 开发功能，不属于默认协议。

服务端口、广播入口、默认/AirPrint 变体和设备验证状态统一见
[固件对外服务状态](../firmware_service_status.md)。

## 协议状态

| 协议 | 默认固件 | 第三方用途 | 入口 |
|---|---|---|---|
| IJPD 控制协议 | 已启用 | 查询设备、创建/取消任务、状态和事件 | TCP `9100`、BLE GATT、可选 MQTT |
| IJPD 二进制数据协议 | 已启用 | 发送 raster 打印数据 | TCP `9101` |
| SoftAP HTTP 配网协议 | 按需启用 | 首次或主动切换 Wi-Fi | `192.168.4.1:80` |
| mDNS/DNS-SD | STA 连网后自动启用 | 发现 IJPD 控制端口 | `_ijpd._tcp` |
| ESC/POS RAW TCP | 当前关闭，可选 | 发送原始 ESC/POS 字节流 | 默认 TCP `9102` |
| MQTT 控制/任务分片 | 当前关闭，可选 | 云端控制和承载 IJPD 数据流 | 用户配置 Broker URI/topic |
| IPP/AirPrint | 默认关闭；需 `AIRPRINT_ENABLE=ON` 显式构建 | 显式构建可启用 query、受限 `image/urf` Print-Job 和 Job 查询，并按需使用 SoftAP 配网；当前接入逻辑 RICOH CMYK8 Mock，不代表真实喷头电气驱动 | 默认无入口 |

USB Serial/JTAG 当前仅用于固件日志控制台，不提供第三方打印协议。

## 默认固件

当前 `main/app_main.cpp` 使用以下配置：

```text
Wi-Fi       enabled
BLE         enabled, device name IJPD-Printer
IJPD TCP    enabled, control 9100, data 9101
RAW TCP     disabled
MQTT        disabled
Profile     RICOH-MH5440-MOCK, A4 logical CMYK8 raster, 2480×3508 @ 300 DPI, 9920 bytes/row
Model       RICOH-MH5440-MOCK
Firmware    0.3.0
```

第三方客户端应优先连接 TCP `9100` 或 BLE，发送 `get_device_info`，再根据
`capabilities` 和 `services` 决定后续路径。不要仅根据
`device.connections` 判断能力；当前固件该字段是兼容性描述，实际服务以
`capabilities.services` 为准。

## 文档索引

1. [IJPD 控制协议](ijpd-control.md)
2. [IJPD TCP 二进制数据协议](ijpd-data-tcp.md)
3. [BLE GATT 控制协议](ble-gatt.md)
4. [SoftAP HTTP 配网协议](provisioning-http.md)
5. [mDNS/DNS-SD 发现协议](mdns.md)
6. [ESC/POS RAW TCP 协议](escpos-raw-tcp.md)
7. [MQTT 控制与 IJCH 任务分片协议](mqtt.md)

## 对接建议

- 控制请求必须保持 TCP/BLE/MQTT 的统一 JSON 信封和 `#` 结束符。
- 大图像必须先 `start_print`，再按响应中的 token 和派生包长发送数据。
- 控制连接不要在收到 `start_print` 响应后立即关闭；进度和终态事件通过控制通道广播。
- 所有多字节二进制字段均为 little-endian。
- 当前默认镜像运行 RICOH MH5440 logical CMYK8 Mock；`get_device_info` 中的能力和硬件状态
  不能当作真实喷头电气规格。
- 默认固件不支持 IPP/AirPrint、PDF 或通过 HTTP 发送打印任务。显式 AirPrint 构建
  可接收已验证的 `image/urf` Print-Job 并送入逻辑 Mock；不支持 PDF，也不能宣称
  已完成真实喷头打印。
