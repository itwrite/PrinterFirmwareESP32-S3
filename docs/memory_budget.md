# 内存与缓冲区预算

> 状态：开发基线；估算项仍需真实喷头 HAL 接入后复测
> 最近更新：2026-10-24
> 原则：数据路径零动态分配；所有缓冲启动期一次性分配，运行期不 malloc/free；
> 所有分配结果必须检查；用 `heap_caps_get_free_size` 与最小水位监控泄漏。

---

## 1. 硬件假设（待确认，见 hardware_questions.md Q1）

| 项 | 假设值 |
|---|---|
| 模组/板卡 | ESP32-S3-WROOM-1 **N16R8**（或等效） |
| Flash | 16 MB（Quad） |
| PSRAM | **8 MB Octal**（esp_psram + cache） |
| 内部 SRAM | 512 KB（其中应用可用约 300 KB，扣除系统/ROM/Cache） |
| CPU | 双核 240 MHz，SMP FreeRTOS |

若实际为 Quad PSRAM / 更小容量：槽位数 `SLOT_COUNT`、缓冲配置按下表等比缩减，并在 Phase 5 前修订本文档。

---

## 2. PSRAM 预算（8 MB Octal）

| 用途 | 单块大小 | 数量 | 小计 | 分配时机 | 内存属性 |
|---|---|---|---|---|---|
| 解压后槽位池（SlotPool） | 65536 B | 6 | 393,216 B | 启动期 | `MALLOC_CAP_SPIRAM` |
| 压缩包接收缓冲（rx↔decode 乒乓） | 65536 B | 2 | 131,072 B | 启动期 | `MALLOC_CAP_SPIRAM` |
| **合计（固定）** | | | **524,288 B (512 KiB)** | | |
| 剩余 PSRAM 堆 | | | ≈ 7,513,088 B (7.17 MiB) | | |

预留策略：

- ≥ 1 MiB 预留给 Phase 7 喷头行缓冲/DMA/波形表（标注"待硬件确认"）。
- PSRAM 不可用（初始化失败/容量不足）：默认**拒绝所有打印任务**（`PSRAM_UNAVAILABLE`）；受限小缓存模式不启用（如需，须显式配置并在 `get_device_info.memory` 上报）。

槽位池设计要点：

- 状态机 `FREE → FILLING → READY → PROCESSING → FREE`，转换仅由 SlotPool 原子接口执行。
- 队列元素只含：槽位索引、包序号、有效长度、任务代次 generation（取消后旧代次元素整体作废）；**不拷贝图像数据**。
- 压缩接收缓冲与槽位池分离，rx_task 获取空闲压缩缓冲 → 填充 → 移交 decode_task → 解压写入槽位 → 归还。两缓冲都忙时 rx_task 暂停读 socket（TCP 背压）。

---

## 3. 内部 SRAM 预算（静态缓冲 + 任务栈）

### 3.1 静态缓冲

| 用途 | 大小 | 说明 |
|---|---|---|
| TCP 控制重组缓冲 | 2 × 2048 B | 最多 2 个控制连接，各一 |
| BLE 重组缓冲 | 1 × 2048 B | 单连接 |
| JSON token 数组 | 3 × 64 × 16 B ≈ 3 KB | 每控制连接一份 |
| JSON 写出器缓冲 | 3 × 1 KB = 3 KB | 分块流式输出 |
| 响应帧缓冲 | 1 × 6144 B = 6 KB | `MAX_RESPONSE_FRAME_SIZE`（device_info 含命令元数据；ctrl 任务独占） |
| 事件队列 | 16 × 64 B = 1 KB | report_task 消费 |
| 状态/统计/杂项 | ≈ 4 KB | |
| **静态小计** | **≈ 22 KB** | |

### 3.2 应用任务栈（初值，Phase 6 用 `uxTaskGetStackHighWaterMark` 实测后固定并加 ≥ 30% 余量）

| 任务 | 栈(字节) | 说明 |
|---|---|---|
| `ctrl_task` | 16384 | select/socket + JSON 解析 + 路由 + AirPrint/mDNS startup |
| `air_start` | 65536 | opt-in AirPrint HTTP/mDNS startup path |
| `rx_task` | 4096 | socket 读 + 帧状态机 + CRC（查表，无大局部数组） |
| `decode_task` | 6144 | LZ4 解压（迭代非递归，局部变量小；余量保守） |
| `print_task` | 4096 | 调度 + HAL 提交 |
| `report_task` | 4096 | 事件序列化 + 发送 |
| **应用栈小计** | **26,624 B ≈ 26 KB** | 以 `task_layout.h` 当前值为准 |

### 3.3 系统占用（估计，以实测为准）

| 项 | 估计 |
|---|---|
| Wi-Fi（含 lwIP、动态 TX/RX） | ≈ 100–150 KB |
| BLE（NimBLE 主机 + 控制器） | ≈ 40–60 KB |
| lwIP TCP 窗口默认（V1 不放大：数据通道目标仅 ≥ 500 KB/s，默认窗口 5760 B×队列即可满足，Phase 6 实测后决定是否调整并记录依据） | 含上 |
| **系统小计** | **≈ 150–200 KB** |

结论：内部 SRAM 应用侧余量充足（静态约 22 KB + 应用栈约 26.6 KB + 系统约
200 KB < 512 KB），但最终余量仍需真实硬件和任务栈高水位实测确认。

### 3.4 当前固件链接基线（ESP-IDF 5.4 / ESP32-S3）

默认镜像 `idf.py size` 当前基线：

| 区域 | 已用 | 剩余 | 备注 |
|---|---:|---:|---|
| IRAM | 16,383 B | 1 B | 专用 IRAM 仍是主要约束 |
| DIRAM | 176,687 B | 165,073 B | 关闭 Wi-Fi RX IRAM 优化后增加余量 |
| Flash image | 1,283,744 B | | 应用分区仍剩约 59% |

当前配置关闭 `CONFIG_ESP_WIFI_RX_IRAM_OPT`，保留
`CONFIG_ESP_WIFI_IRAM_OPT`。实测释放约 9.6 KiB DIRAM，同时镜像增加约
8.8 KiB。该取舍需要通过真实 Wi-Fi TCP 吞吐和延迟基准确认。

---

## 4. FreeRTOS 任务布局（SMP 初值）

| 任务 | 核心 | 优先级 | 职责 | 关键同步对象 | 共享状态所有权 |
|---|---|---|---|---|---|
| Wi-Fi/lwIP/BLE | Core 0 | 系统设定 | 不干预 | — | — |
| `rx_task` | 0 | 10 | 读数据 socket、帧状态机、顺序/CRC 校验、填充压缩缓冲 | 压缩缓冲池信号量、槽位水位事件、取消事件 | 仅写压缩缓冲内容 |
| `decode_task` | 1 | 12 | LZ4/拷贝入槽、槽位状态推进 | 满缓冲队列、槽位事件、取消事件 | 仅驱动 FILLING→READY |
| `print_task` | 1 | 14（应用层最高） | 按行/带向 HAL 提交、走纸协调、完成判定 | READY 槽位队列、HAL 空闲信号量、取消事件 | 仅驱动 READY→PROCESSING→FREE |
| `ctrl_task` | 0 | 4 | 控制帧→JSON→路由→响应（BLE+TCP 共用） | 每连接发送互斥 | job_state_machine 调用者 |
| `report_task` | 0 | 3 | 事件序列化与广播发送 | 事件队列 | 无共享可变状态 |

规则：

- 应用任务优先级全部 < 18，低于系统任务；`print_task` 为应用层最高，**不做 JSON 序列化、不做阻塞网络发送、不做日志刷屏**。
- `job_state_machine` 所有转换单点实现，内部互斥；generation 为原子量。
- 所有阻塞等待使用有限超时 + 取消事件可唤醒；打印期间 rx/decode/print 订阅 TWDT，`print_task` 异常触发 HAL `stop()` 安全停机。
- rx↔decode 合并为单任务的方案被否决：LZ4 解压与 CRC/收包并行可隐藏 ~毫秒级延迟，且职责分离便于压测（对应任务书 7.3 备注的说明要求）。

---

## 5. 流控与水位

- 高/低水位按"READY + PROCESSING 槽位数"计：`HIGH_WATERMARK=4`，`LOW_WATERMARK=2`（6 槽，滞回防抖）。
- 达到高水位：`rx_task` 停止从 socket 读取 → TCP 窗口收缩形成背压；回落到低水位恢复。
- TCP 背压 ≠ 应用层 ACK；同时每 200 ms / 每 10 包广播 `print_progress`，APP 据此自限发送窗口。
- 队列满时不覆盖、不无限阻塞，`cancel_print` 必须随时可响应。

---

## 6. 吞吐可行性（数量级校验）

| 环节 | 需求 | S3 能力（数量级） | 结论 |
|---|---|---|---|
| Wi-Fi TCP 有效吞吐 | ≥ 500 KB/s | 数 MB/s | 可行 |
| CRC32 查表 | 与接收同速 | 数百 MB/s @240 MHz | 可行 |
| LZ4 解压 | ≥ 500 KB/s | ≈ 20–40 MB/s | 可行 |
| PSRAM 写（cache） | ≥ 500 KB/s | 数十 MB/s | 可行 |
| 真实瓶颈 | — | 喷头机械/热极限（未知，Phase 7） | 上游永远比下游快，靠水位背压适配 |

---

## 7. DMA 与 Cache 一致性（标注：待硬件确认）

- 当前所有 PSRAM 缓冲仅由 CPU 访问，无 DMA 参与,V1 无一致性风险。
- Phase 7 若喷头路径引入 SPI/LCD_CAM + GDMA：需重新评估缓冲位置（内部 SRAM 或 PSRAM-DMA 支持）、Cache 回写/失效、对齐（`MALLOC_CAP_DMA` / 32 B 对齐）。**届时本文档必须先行修订，再写驱动代码。**

---

## 8. 监控项（`stats` 模块，Phase 4 实现）

- 队列当前水位与历史最大水位；包计数（收/解/打）；CRC/解压失败计数；吞吐率 B/s。
- `heap_caps_get_free_size(MALLOC_CAP_SPIRAM / INTERNAL)` 与启动以来最小水位（连续 100 次 Mock 任务后不得持续下降——验收项）。
- 各任务栈高水位（Phase 6 实测记录进本文档）。
