# IJPD 通用打印机协议中间件

> **定位**：让任何打印机（热敏/喷墨/标签/针式）接入统一 App 生态。
> 中间件解决**指令协议 + 传输协议 + 数据流水线**，打印机厂商只需提供 Profile + HAL。
>
> **版本**：0.2.0（中间件化）· ESP-IDF v5.4.1 · C++17（无异常/无 RTTI）
> **核心原则**：自研优先（`third_party/` 为空）、零第三方依赖、Profile + HAL 插件化
>
> 📖 **[中间件架构与集成指南](docs/middleware.md)** ← 打印机厂商从这里开始

---

## 功能状态（三级标注）

### ✅ 已完成（主机验证与 ESP32-S3 真机联调）

| 功能 | 验证方式 |
|---|---|
| `#` 拆帧（半包/粘包/空帧/超限/超时） | 单测 + fuzz 10 万次 |
| JSON 子集解析（严格模式/UTF-8/代理对/深度/长度/溢出） | 单测 ~150 断言 + fuzz |
| JSON 流式写出（`#`→`\u0023` 强制转义/溢出不截断） | 单测 ~226 断言 |
| 六命令路由 + 参数校验 + 语义交叉 + 元数据同源 | 单测 ~192 断言 |
| 设备信息上报（PSRAM 余量/能力/限制/命令元数据） | 回环断言 ~31 |
| 通用打印头档案驱动 | Profile 声明布局、通道顺序/位深、宽高限制和逐通道打印属性；IJPD 能力读取、任务校验、光栅转换与 HAL 分发均按档案运行 |
| IJPD CMYK1 四平面打印路径 | ESP32-S3 + iPhone 真机验证；4.1 MB 光栅传输并完成打印约 17 秒 |
| 打印进度与速率修复 | 修复 Mock 每行毫秒取整限速、1-bit 通道逐像素处理开销，以及传输完成事件导致的打印进度回退 |
| CRC-32/ISO-HDLC | 标准向量 + 协议示例 |
| 二进制帧状态机（Hello/顺序/长度/CRC/NeedBuffer） | 单测 ~230 + fuzz 10 万 |
| LZ4 Block 解压（安全边界/重叠拷贝/恶意输入） | 手工向量 + 回环 + fuzz 20 万 |
| 槽位池（MPMC/代次取消/背压/无泄漏） | 多线程压测 + TSan |
| 任务状态机（11 态 132 组合表驱动全覆盖） | 表驱动 + 并发竞争 |
| 流控（高低水位滞回） | 单测 |
| 统计（计数/水位/吞吐窗口） | 单测 + 4 线程并发 |
| 控制面汇入（TCP×2+BLE 三通道独立拆帧/超时/超长） | 单测 |
| 打印流水线闭环（跨包行重组/完成 5 条件/取消/致命/收尾） | **端到端 387 断言 + 连续 100 任务** |
| 事件序列化（print_progress / print_finished）与广播 | 流水线事件断言 |
| Python LZ4 编码器/解码器 + 分包生成器 + 模拟客户端 | selftest 50 用例 + 固件交叉验证 |

### 🔶 Mock 实现（协议正确，硬件为模拟）

| 功能 | Mock 行为 | 真实实现 |
|---|---|---|
| 喷头（Mock HAL） | 默认 IJPD Profile 为四个 1-bit 平面，依 C/M/Y/K 顺序模拟逐通道输出；支持逻辑统计、CRC 和故障注入 | Phase 7：需厂商时序/电压/波形 |
| 走纸（`MockStepperHal`） | 按距离/速度模拟延时 | Phase 7：GPTimer 脉冲+梯形加减速 |
| 切纸（`MockCutterHal`） | 恒返回 `UNSUPPORTED` | Phase 7：确认机构后启用 |

### 🔒 待硬件验证 / 阻塞项

| 项 | 阻塞原因 | 解锁条件 |
|---|---|---|
| 真实喷墨打印 | 喷头型号/时序/波形未知（`docs/hardware_questions.md` B 组） | 用户提供官方资料 |
| 真实走纸/切纸 | 电机/传感器型号未知（C/D 组） | 同上 |
| Wi-Fi / BLE / TCP 设备行为 | IJPD TCP 控制/数据通道及 iPhone 到 ESP32-S3 的完整逻辑 Mock 打印链路已验证 | 真实喷头/机械硬件 |
| 物理打印头与纸路 | 当前打印由 Mock HAL 模拟；真机链路测试不代表纸张输出或真实喷头驱动 | 取得喷头、电机和传感器资料并完成真实 HAL |
| 任务栈高水位 | 使用初值 | `uxTaskGetStackHighWaterMark` 实测后固定 |
| Wi-Fi 配网方式 | 当前为 SoftAP HTTP 门户 | 如需替换再改平台层 |

### 通用固件配置

通过 `core/printer_config.h` 的 `PrinterConfig` 选择打印机变体。Profile、设备型号
和服务开关均可按硬件配置；未声明的服务默认关闭。当前默认镜像启用 IJPD TCP、BLE
控制和 TCP 数据通道；ESC/POS RAW TCP、MQTT 为可选服务。AirPrint Core 和
query/Print-Job service 仅在 `AIRPRINT_ENABLE=ON` 的显式构建中包含，默认镜像不
监听 TCP 631 或发布 IPP mDNS。

当前默认 IJPD Profile 是 **逻辑 CMYK1 四遍 Mock**：2480 × 3508 像素、300 DPI，
C/M/Y/K 四个 1-bit 平面依次传输和模拟打印，每个平面 310 bytes/row。iPhone 与
ESP32-S3 真机联调中，约 4.1 MB 任务打印完成耗时约 17 秒。该数据用于记录当前 Mock
数据路径性能，不代表真实喷头速度；设备能力与打印数据格式由通用 `PrintProfile`
声明，App 不按具体喷头型号硬编码转换规则。

---

## 目录结构

```text
.
├── CMakeLists.txt            # ESP-IDF 顶层
├── sdkconfig.defaults        # esp32s3 / Octal PSRAM / NimBLE 外设 / TWDT
├── partitions.csv            # 16MB：nvs + phy + factory(3MB)
├── main/                     # app_main + Kconfig
├── components/
│   ├── core/                 # 平台无关纯 C++（14 模块，可主机编译，零 IDF 依赖）
│   ├── platform_esp/         # Wi-Fi / TCP×2 / BLE(NimBLE) / PSRAM / 任务 / 日志
│   ├── printer_hal/          # IPrintheadHal / IStepperHal / ICutterHal + Ricoh/Mock HALs
│   │                         # 注：任务书 hal/ 与 IDF 内置组件 hal 同名冲突，更名
│   ├── app/                  # control_plane / device_info / print_service / printer_app
│   ├── airprint_core/        # opt-in 平台无关 IPP/HTTP/Job Core 和样本契约
│   └── airprint_esp/         # opt-in ESP-IDF HTTP/mDNS/query/Print-Job transport
├── host_tests/               # 主机测试（自写断言 + ASan/UBSan/TSan + fuzz 固定种子）
├── tools/                    # make_packets.py / send_job.py（仅 Python 标准库）
└── docs/                     # 协议 / 内存预算 / 决策 / 硬件问题 / 测试报告 / 依赖审计
```

## 快速开始

### 主机测试（无需硬件，~90s）

```bash
cmake -S host_tests -B host_tests/build
cmake --build host_tests/build
ctest --test-dir host_tests/build --output-on-failure
```

TSan（并发模块改动后建议运行）：

```bash
cmake -S host_tests -B host_tests/build_tsan -DIJPD_SANITIZER=thread
cmake --build host_tests/build_tsan
ctest --test-dir host_tests/build_tsan -R "slot_pool|state_machine|stats|print_pipeline"
```

### 设备构建与烧录（需 ESP-IDF v5.4.1）

```bash
source ~/esp/esp-idf/export.sh
idf.py set-target esp32s3
idf.py menuconfig                    # 基础 IDF 配置；首次联网使用 SoftAP 配网
idf.py build                         # 固件构建；现有平台组件仍有既有 warning
idf.py -p <PORT> flash monitor       # 需真实设备
```

显式构建 AirPrint query/Print-Job Mock 服务（支持按需 SoftAP 配网）：

```bash
tools/idf.sh -B build-airprint-probe build -DAIRPRINT_ENABLE=ON
```

真机联调步骤见 [docs/airprint_probe_runbook.md](docs/airprint_probe_runbook.md)。

### 端到端联调（需设备在线）

```bash
python3 tools/send_job.py --host <设备IP> --bytes 120000 --packets 30 --pattern mixed
python3 tools/send_job.py --host <设备IP> --cancel-after 10    # 中途取消
python3 tools/send_job.py --host <设备IP> --corrupt-crc        # CRC 损坏
python3 tools/send_job.py --host <设备IP> --break-after 12     # 断开数据连接
python3 tools/send_job.py --host <设备IP> --never-connect       # 数据连接超时
```

BLE：搜索设备名 `IJPD-Printer`，UUID 和分片规则见
`docs/protocols/ble-gatt.md`。

## 故障排查

| 症状 | 排查 |
|---|---|
| `idf.py build` 报 PSRAM 头文件找不到 | 确认 `idf.py set-target esp32s3` 已执行；sdkconfig.defaults 被读取 |
| PSRAM 初始化失败 | 板卡可能非 Octal PSRAM（改 `CONFIG_SPIRAM_MODE_QUAD=y`）；确认模组带 PSRAM |
| Wi-Fi 不连接 | 连接 `IJPD-Setup-XXXX` 并访问 `http://192.168.4.1` 完成配网；日志 `wifi:` 标签查看重连退避 |
| BLE 搜不到 | NimBLE 仅外设角色；确认设备名 `IJPD-Printer`；广播在同步后自动启动 |
| TCP 连不上 | 确认设备与主机同一网段；控制端口 9100，数据端口 9101 |
| `PSRAM_UNAVAILABLE` 响应 | `get_device_info` 检查 `memory.psram_available`；启动日志 `psram:` 标签 |
| 编译告警 | 我们的组件启用 `-Werror`；用与 README 相同的 IDF 版本（v5.4.1） |

## 文档索引

完整文档导航见 [docs/README.md](docs/README.md)。

### 第三方协议对接（当前实现）

| 文档 | 内容 |
|---|---|
| [docs/firmware_service_status.md](docs/firmware_service_status.md) | 当前固件所有对外服务、端口、广播、默认/AirPrint 变体状态 |
| [docs/protocols/README.md](docs/protocols/README.md) | 协议总览、默认启用状态和对接路径 |
| [docs/protocols/ijpd-control.md](docs/protocols/ijpd-control.md) | TCP/BLE/MQTT 共用的 IJPD JSON 控制协议 |
| [docs/protocols/ijpd-data-tcp.md](docs/protocols/ijpd-data-tcp.md) | TCP 9101 Hello、二进制分包、CRC32、LZ4 |
| [docs/protocols/ble-gatt.md](docs/protocols/ble-gatt.md) | BLE GATT UUID、分片和通知协议 |
| [docs/protocols/provisioning-http.md](docs/protocols/provisioning-http.md) | SoftAP HTTP 配网接口 |
| [docs/protocols/mdns.md](docs/protocols/mdns.md) | `_ijpd._tcp` mDNS/DNS-SD 发现 |
| [docs/protocols/escpos-raw-tcp.md](docs/protocols/escpos-raw-tcp.md) | 可选 ESC/POS RAW TCP 字节流 |
| [docs/protocols/mqtt.md](docs/protocols/mqtt.md) | 可选 MQTT 控制、事件和 IJCH 任务分片 |

以上文档以当前源码为准。默认固件不提供 IPP/AirPrint；显式 AirPrint variant
可把验证过的 `image/urf` 送入独立的逻辑 CMYK8 Mock 路径。默认 IJPD Profile 使用
CMYK1 四平面 Mock。两者都尚未接入真实喷头电气驱动。

### 工程设计和测试

| 文档 | 内容 |
|---|---|
| [docs/middleware.md](docs/middleware.md) | 打印机厂商接入 Profile/HAL 的简明指南 |
| [RICOH_MH5440_Mock_HAL.md](RICOH_MH5440_Mock_HAL.md) | RICOH MH5440 逻辑 Mock、故障注入和未来真实 HAL 迁移边界 |
| [docs/memory_budget.md](docs/memory_budget.md) | PSRAM/SRAM 分配表 + 任务布局 |
| [docs/hardware_questions.md](docs/hardware_questions.md) | 待确认硬件问题清单 |
| [docs/test_report.md](docs/test_report.md) | 测试矩阵 / 当前验证结果 |
| [docs/dependency_audit.md](docs/dependency_audit.md) | 依赖审计（third_party/ 为空） |
| [docs/clion_setup.md](docs/clion_setup.md) | CLion、ESP-IDF、烧录和调试环境 |
| [docs/TODO.md](docs/TODO.md) | 当前未完成事项和后续工作 |
| [tools/README.md](tools/README.md) | 工具用法 |

## 安全声明

- 真实喷头驱动时序/电压/点火波形**未实现**，在用户提供官方资料前仅 Mock，不生成点火代码。
- A4988 仅用于兼容步进电机走纸，绝不作为喷头驱动器。
- 切纸硬件未确认前 `cut_paper` 返回 `UNSUPPORTED`，不驱动任何 GPIO。
- 令牌/密码不出现在日志中；Wi-Fi 凭据由 SoftAP 配网门户保存到 NVS。
