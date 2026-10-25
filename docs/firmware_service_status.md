# 固件对外服务状态

> 当前固件对外服务、端口和广播入口的状态概览。以当前源码配置和最近一次设备验证为准。

## 固件变体

| 固件变体 | 构建方式 | 当前用途 |
|---|---|---|
| 默认固件 | `tools/idf.sh -B build build` | IJPD TCP、BLE 和常规 Wi-Fi 固件 |
| AirPrint probe | `tools/idf.sh -B build-airprint-probe build -DAIRPRINT_ENABLE=ON` | AirPrint/iOS 联调，支持按需 SoftAP 配网 |

两个变体当前都使用 `RICOH-MH5440-MOCK` / `MockRicohMh5440Hal`，不代表真实喷头和真实纸张输出。

## 服务状态

| 服务 | 默认固件 | AirPrint probe | 对外入口 | 用途 |
|---|---|---|---|---|
| Wi-Fi STA | 已启用 | 已启用 | 局域网 IP | TCP、mDNS、AirPrint 的网络承载 |
| IJPD 控制 TCP | 已启用 | 已启用 | TCP `9100` | 设备查询、创建/取消任务、状态和事件 |
| IJPD 数据 TCP | 按任务启用 | 按任务启用 | TCP `9101` | raster 数据传输；由 `start_print` 返回端口 |
| IJPD mDNS/DNS-SD | STA 获得 IP 后启用 | STA 获得 IP 后启用 | `_ijpd._tcp.local` | 发现 IJPD 控制服务，指向 TCP `9100` |
| BLE GATT 控制 | 已启用 | 已启用 | BLE `IJPD-Printer` | IJPD 控制协议；不承载 raster 数据 |
| SoftAP 配网 Wi-Fi | 按需启用 | 按需启用 | SSID `IJPD-Setup-XXXX` | 首次或失败后的 Wi-Fi 配网 |
| SoftAP 配网 HTTP | 按需启用 | 按需启用 | `192.168.4.1:80` | 配网页面、扫描和提交 Wi-Fi 凭据 |
| 主动切换配网 | `start_provisioning` | `start_provisioning` | IJPD 控制 TCP/BLE/MQTT | 已保存 Wi-Fi 时主动切换到 SoftAP；活动传输中拒绝 |
| ESC/POS RAW TCP | 关闭 | 关闭 | 默认 TCP `9102` | 原始 ESC/POS 字节流；仅固件变体可选启用 |
| MQTT 控制/任务分片 | 关闭 | 关闭 | 配置的 MQTT Broker | 云端控制和任务数据承载 |
| IPP/AirPrint | 关闭 | 已启用 | TCP `631`，`/ipp/print` | AirPrint 查询、Print-Job 和 Job 查询 |
| AirPrint mDNS/DNS-SD | 关闭 | 已启用 | `_ipp._tcp.local` | 发现 AirPrint 服务 |
| AirPrint universal subtype | 关闭 | 已启用 | `_universal._sub._ipp._tcp` | AirPrint discovery subtype，指向同一 IPP 实例 |
| USB Serial/JTAG | 调试接口 | 调试接口 | `/dev/cu.usbmodemXXX` | 日志、烧录和监视，不提供打印协议 |

## IJPD TCP

| 入口 | 状态 | 说明 |
|---|---|---|
| TCP `9100` 控制通道 | 已启用 | JSON 控制帧，使用 `#` 作为帧结束符 |
| TCP `9101` 数据通道 | 按任务启用 | 每个任务建立一个数据连接，支持 IJPD 二进制分包、CRC32 和 LZ4 |
| mDNS `_ijpd._tcp` | STA 后启用 | 只广播控制端口 `9100`，数据端口从控制响应中获取 |

## BLE GATT

| 对象 | 状态/值 |
|---|---|
| 广播名称 | `IJPD-Printer` |
| 广播 Service UUID | `5a8f0001-7c2b-4d6e-9a01-3f5e8d7c2b01` |
| Write characteristic | `5a8f0002-7c2b-4d6e-9a01-3f5e8d7c2b01` |
| Notify characteristic | `5a8f0003-7c2b-4d6e-9a01-3f5e8d7c2b01` |
| 控制内容 | IJPD JSON 控制帧 |
| 打印数据 | 不通过 BLE，使用 TCP `9101` 或可选 MQTT |
| 当前运行状态 | 广播启动已验证；手机端 GATT 回环待验证 |

## AirPrint

| 项目 | 默认固件 | AirPrint probe |
|---|---|---|
| TCP 端口 | 关闭 | `631` |
| HTTP 路径 | 无 | `POST /ipp/print` |
| `_ipp._tcp` mDNS | 关闭 | 已启用 |
| `Get-Printer-Attributes` | 关闭 | 已启用 |
| `Print-Job` | 关闭 | 已启用 |
| `Validate-Job` | 关闭 | 已启用 |
| `Get-Jobs` | 关闭 | 已启用 |
| `Get-Job-Attributes` | 关闭 | 已启用 |
| `Cancel-Job` | 关闭 | 已启用 |
| 文档格式 | 无 | `image/urf`、`application/octet-stream` |
| 后端 | 无 | RICOH CMYK8 logical Mock |

## 端口和广播速查

| 入口 | 默认固件 | AirPrint probe |
|---|---:|---:|
| TCP `80` SoftAP 配网 | 按需 | 按需 |
| TCP `631` IPP/AirPrint | 关闭 | 已启用 |
| TCP `9100` IJPD 控制 | 已启用 | 已启用 |
| TCP `9101` IJPD 数据 | 按任务 | 按任务 |
| TCP `9102` ESC/POS RAW | 关闭 | 关闭 |
| BLE GATT | 已启用 | 已启用 |
| `_ijpd._tcp` mDNS | STA 后启用 | STA 后启用 |
| `_ipp._tcp` mDNS | 关闭 | 已启用 |
| `_universal._sub._ipp._tcp` | 关闭 | 已启用 |
| MQTT | 关闭 | 关闭 |

## 最近设备状态

| 项目 | 值 |
|---|---|
| 设备 | ESP32-S3 |
| 固件变体 | AirPrint probe |
| 固件版本字段 | `0.3.0` |
| MAC | `10:20:ba:71:d5:d0` |
| 当前网络 | SoftAP |
| SoftAP SSID | `IJPD-Setup-D5D1` |
| SoftAP IP | `192.168.4.1` |
| 配网 HTTP | `192.168.4.1:80` |
| USB 串口 | `/dev/cu.usbmodem101` |
| LCD | ILI9341 初始化通过 |
| PSRAM | 初始化和内存测试通过 |
| AirPrint Print-Job | HTTP `200` / IPP `successful-ok` |
| 最近 Job | `completed`, `pages-completed=1` |

## 相关文档

| 文档 | 内容 |
|---|---|
| [协议总览](protocols/README.md) | IJPD、BLE、SoftAP、mDNS、RAW TCP、MQTT 协议 |
| [AirPrint 联调](airprint_probe_runbook.md) | AirPrint 构建、刷写和联调步骤 |
| [测试报告](test_report.md) | 主机测试、固件构建和设备验证记录 |
