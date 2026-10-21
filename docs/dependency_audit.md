# 依赖审计报告（Phase 8）

> 依据：任务书第二章《自研优先与依赖政策》

## 1. `third_party/` 目录

**不存在（为空）。未引入任何第三方库。《依赖申请表》：零份。**

## 2. 实际使用的外部代码

### L0：ESP-IDF v5.4.1 内置组件（允许直接使用）

| 组件/模块 | 许可证 | 用途 |
|---|---|---|
| FreeRTOS（SMP） | MIT | 任务/队列/信号量/调度 |
| lwIP | BSD-3-Clause | TCP socket（控制 9100 / 数据 9101） |
| esp_wifi / esp_netif / esp_event | Apache-2.0 | Wi-Fi STA 连接管理 |
| NimBLE（`bt` 组件） | Apache-2.0 | BLE GATT 外设服务 |
| esp_psram / esp_heap_caps（heap） | Apache-2.0 | PSRAM 探测/分配 |
| esp_timer | Apache-2.0 | 毫秒时间源 / Wi-Fi 重连退避 |
| nvs_flash | Apache-2.0 | Wi-Fi 校准数据 / NimBLE 持久化 |
| esp_hw_support（efuse/MAC） | Apache-2.0 | 设备 ID（esp_read_mac） |
| esp_random | Apache-2.0 | data_token 生成（硬件 RNG） |
| ESP-IDF 构建/工具链 | Apache-2.0 | CMake / idf.py / xtensa-gcc |

ESP-IDF 总体许可证：Apache-2.0（允许闭源商用，符合任务书 2.3 准入标准第 1 条）。
锁定版本：**v5.4.1**（`git describe` 于 `~/esp/esp-idf`）。

### L2：仅开发机（不入固件产物）

| 工具 | 许可证 | 用途 |
|---|---|---|
| Python 3 标准库（socket/struct/json/zlib/random/argparse/select/subprocess/tempfile） | PSF | `tools/make_packets.py`（含自研 LZ4 编码器）、`tools/send_job.py`、`selftest` |
| 官方 LZ4（**未使用**） | — | 可选交叉验证（当前用固件同源 `lz4_check` 二进制，不引入外部代码） |
| CMake / clang / AppleClang | BSD / Apache-2.0 | 主机构建与测试 |

## 3. 自研模块清单（对应任务书 2.2 逐项）

| 模块 | 文件 | 状态 |
|---|---|---|
| 控制帧拆分器 | `core/frame_splitter` | ✅ |
| JSON 子集解析器 | `core/json_lite`（解析+写出） | ✅ |
| JSON 写出器 | 同上 | ✅ |
| 二进制帧状态机 | `core/bin_frame_parser` | ✅ |
| LZ4 Block 解压器 | `core/lz4_block` | ✅ |
| CRC32 | `core/crc32` | ✅ |
| 环形缓冲/槽位池 | `core/slot_pool`（Vyukov MPMC + CAS 状态机） | ✅ |
| 打印状态机 | `core/job_state_machine`（11 态表驱动） | ✅ |
| 流控 | `core/flow_control`（高低水位滞回） | ✅ |
| 统计 | `core/stats` | ✅ |
| 打印流水线 | `core/print_pipeline`（三泵模型） | ✅ |
| HAL 抽象 + Mock | `printer_hal/`（3 接口 + 3 Mock） | ✅ Mock（真实待 Phase 7） |
| 步进电机梯形加减速 | — | 🔒 Phase 7（待硬件参数） |

## 4. 结论

- **零第三方依赖引入固件产物。**
- 所有自研模块通过主机侧单元测试 + fuzz + TSan（见 `docs/test_report.md`）。
- 工程结构符合"core 不含 ESP-IDF 头文件 / hal 仅接口"铁律。
