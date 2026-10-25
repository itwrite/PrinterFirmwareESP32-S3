# 文档入口

只维护以下几类文档。第三方对接、固件开发和硬件接入分别从对应入口进入，
不要在整个 `docs/` 目录中搜索旧协议稿或把 opt-in AirPrint 草稿当作默认协议。

## 先看哪份

| 你的目标 | 入口 |
|---|---|
| 查看当前固件所有对外服务和端口状态 | [firmware_service_status.md](firmware_service_status.md) |
| App/PC/云端对接打印机 | [protocols/README.md](protocols/README.md) |
| 为新打印机接入 Profile/HAL | [middleware.md](middleware.md) |
| RICOH MH5440 无硬件时做 Mock | [../RICOH_MH5440_Mock_HAL.md](../RICOH_MH5440_Mock_HAL.md) |
| 确认喷头、电机、切刀等硬件问题 | [hardware_questions.md](hardware_questions.md) |
| 了解 PSRAM、任务栈和流控预算 | [memory_budget.md](memory_budget.md) |
| 查看测试矩阵和验证结果 | [test_report.md](test_report.md) |
| 查看 ESP-IDF/CLion 烧录调试 | [clion_setup.md](clion_setup.md) |
| 查看依赖和许可证 | [dependency_audit.md](dependency_audit.md) |
| 查看未完成事项 | [TODO.md](TODO.md) |
| AirPrint query / Print-Job 真机联调 | [airprint_probe_runbook.md](airprint_probe_runbook.md) |

## 第三方协议

从 [protocols/README.md](protocols/README.md) 开始。其下按承载和功能拆分：

- `ijpd-control.md`：TCP/BLE/MQTT 共用的 JSON 控制协议。
- `ijpd-data-tcp.md`：TCP `9101` Hello、二进制分包、CRC32、LZ4。
- `ble-gatt.md`：BLE Service/Characteristic UUID 和分片。
- `provisioning-http.md`：SoftAP HTTP 配网。
- `mdns.md`：`_ijpd._tcp` 发现。
- `escpos-raw-tcp.md`：可选 RAW ESC/POS 字节流。
- `mqtt.md`：可选 MQTT 控制、事件和 IJCH 任务分片。

默认固件不提供 IPP/AirPrint。AirPrint Core、host Apple Raster decoder 子集和
opt-in ESP-IDF transport 只在显式构建中编译；显式 variant 已接入 logical Mock，
不代表真实喷头打印。

## 维护规则

- 协议行为以 `docs/protocols/` 和当前源码为准。
- 新增协议先新增独立文档，再在 `protocols/README.md` 登记。
- 设计草稿、阶段决策和已被替代的协议版本不再放入 `docs/`。
- 代码、脚本或测试中的文档链接必须指向上述保留文档。
