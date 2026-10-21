# Phase 0 决策报告：协议与工程决策

> 项目：ESP32-S3 DIY 6 英寸彩色喷墨照片打印机固件
> 日期：2026-10-15
> 状态：**Phase 0 完成，等待用户确认后进入 Phase 1**
> 上游依据：《ESP32-S3_喷墨打印机固件开发AI提词_V1.0.md》（下称"任务书"）

---

## 1. 已知条件（无需再确认）

| # | 条件 |
|---|---|
| K1 | 全新项目，从零开发；工作区无既有工程（已核实，仅存在任务书文档） |
| K2 | 目标平台 ESP32-S3 + ESP-IDF 原生 CMake；语言 C++17，禁异常/RTTI；数据路径零动态分配 |
| K3 | 通信架构：BLE + TCP 双控制通道（`#` 结尾 JSON）、独立 TCP 二进制数据通道（令牌握手） |
| K4 | 二进制分包：LE、6B 头、CRC-32/ISO-HDLC 可选、LZ4 Block 独立压缩 + 存储兜底规则 |
| K5 | 流水线：rx → 帧状态机 → 校验 → 解压 → PSRAM 槽位池(6×64KB) → 打印调度 → HAL |
| K6 | 任务状态机 11 态：IDLE/PREPARING/WAITING_DATA_CONN/RECEIVING/DRAINING/FINALIZING/COMPLETED/DONE_WITH_WARNING/CANCELLING/CANCELLED/ERROR；槽位 5 态：FREE/FILLING/READY/PROCESSING/FREE |
| K7 | 错误码 23 条（任务书第十章全集，已在 protocol_v1.md §3.3 定稿映射） |
| K8 | 命令白名单 6 条：get_device_info/start_print/cancel_print/get_print_status/feed_paper/cut_paper |
| K9 | 分层铁律：`core/` 零 ESP-IDF 依赖（可主机编译测试）；`hal/` 只暴露接口；`platform_esp/` 承接全部 IDF 依赖 |
| K10 | Mock HAL 为默认模式；真实喷头驱动未获官方资料前不生成任何点火时序代码 |
| K11 | 主机测试：g++/clang + 自写断言 + ASan/UBSan；fuzz 固定种子 ≥ 10 万次迭代 |
| K12 | TWDT 默认启用；打印任务卡死必须能安全停机（停点火、停电机） |

## 2. 未知条件与默认假设

| # | 未知项 | 默认假设（安全侧） | 闭合时点 |
|---|---|---|---|
| U1 | 板卡/Flash/PSRAM 型号 | N16R8（16MB Flash + 8MB Octal PSRAM） | Phase 5 前 |
| U2 | 喷头全部参数（型号/时序/喷嘴映射/温控） | MockPrintheadHal；device_info 中全部 `null`/`is_configured:false` | Phase 7 前 |
| U3 | 走纸电机与传动、限位 | MockStepperHal；feed_paper 模拟执行 | Phase 7 前 |
| U4 | 切纸机构 | 不存在：`UNSUPPORTED` + `supported:false` | Phase 7 前 |
| U5 | 像素格式/喷嘴映射/行序 | 协议层透传（bytes_per_row×total_rows 校验），真实解释推迟 | Phase 7 前 |
| U6 | Wi-Fi 配网方式 | **开放决策 D-11**，Phase 5 前必须确认 | Phase 5 前 |
| U7 | 已安装 ESP-IDF 具体版本 | 推荐 **v5.3.x LTS**（Phase 5 锁定补丁版本写入 README） | Phase 5 前 |
| U8 | 数据通道吞吐需求 | ≥ 500 KB/s（6 寸照片 ≤ 64MB，按分钟级打印估算） | Phase 6 实测校核 |

完整问题表见 `docs/hardware_questions.md`。

## 3. 框架选型确认：ESP-IDF 原生 CMake（决策 D-1）

**结论：确认任务书选型，无替代方案提案。**

理由（任务书 4.1 全部成立，逐条确认）：

1. NimBLE/lwIP/FreeRTOS SMP/GPTimer/LCD_CAM/SPI-GDMA 均为 IDF 原生组件，零第三方封装层；
2. 可精确控制绑核、`MALLOC_CAP_*` 内存属性与中断优先级——喷墨打印的硬实时路径必需；
3. `idf.py` 依赖透明、可锁版本、可审计；
4. 喷头最终需要定时器/DMA/专用外设精确时序，Arduino 层无法胜任且引入不可控抽象。

排除项：Arduino-ESP32 / PlatformIO（多余抽象、不利实时与内存控制）、NimBLE-Arduino（用 IDF 内置栈）——与任务书 2.4 一致。

编译纪律：`-Wall -Wextra -Werror`；core 层 `-fno-exceptions -fno-rtti`；核心路径禁用 `std::string/vector/map` 等隐式堆分配容器（允许 `<cstdint>/<cstring>/<array>/<atomic>`）。

## 4. 自研模块清单、工作量与风险评估（决策 D-3）

| 模块 | 预估规模 | 风险 | 风险点与对策 | 测试策略 |
|---|---|---|---|---|
| `crc32` | ~60 行 | 低 | 查表生成需验证 | 标准向量 + 单比特翻转 |
| `frame_splitter` | ~120 行 | 低 | 半包/粘包/超限/超时 | 逐字节投喂 + 随机切分 |
| `json_lite`（解析+写出） | ~700 行 | **中** | UTF-8 校验、`\uXXXX`、严格模式、写出溢出 | 向量 + fuzz + 截断测试 |
| `command_schema` + `command_router` | ~500 行 | 中低 | 元数据/校验同源 | 表驱动 + 未知字段/类型矩阵 |
| `bin_frame_parser` | ~250 行 | 中低 | 状态机边界、长度校验次序 | 半包/粘接/随机切分 + fuzz |
| `lz4_block` | ~180 行 | **中** | 长度扩展溢出、offset 越界、重叠拷贝 | 手工向量 + 回环 + ASan 恶意输入 |
| `slot_pool` | ~220 行 | **中** | SMP 竞态、状态覆盖 | 主机多线程压力 + 取消注入 |
| `flow_control` | ~120 行 | 低 | 水位滞回 | 高低水位边界测试 |
| `job_state_machine` | ~350 行 | 中 | 非法转换、终态保持 | 表驱动全转换覆盖 |
| `stats` | ~150 行 | 低 | — | 计数正确性 |
| Mock HAL ×3 | ~250 行 | 低 | 必须显式命名 Mock，禁止伪实现 | 延迟/故障注入参数化 |
| `tools/*.py`（仅标准库） | ~400 行 | 低 | LZ4 简易编码器正确性 | 与固件解压器回环互验 |
| 主机测试 + fuzz | ~1500 行 | 中 | 覆盖度 | 见 §8 验收 |

合计约 5000 行（含测试）。工作量集中在 json_lite 与并发流水线，均有明确对策；无单点不可行项。

## 5. 依赖清单（决策 D-4/D-5）

- **`third_party/` 为空。无任何《依赖申请表》。**
- L0（IDF 自带，允许）：FreeRTOS、lwIP、esp_wifi、NimBLE（IDF 内置 bt 组件）、esp_psram/heap_caps、driver（GPTimer/SPI/RMT/LCD_CAM/MCPWM/PCNT/ETM）、esp_timer、NVS、esp_system（TWDT）、Unity（仅设备侧少量集成测试）。
- L2（仅开发机，不入固件）：Python 3 标准库（socket/struct/json/zlib）；官方 LZ4 命令行**可选**用于交叉验证，不进入仓库构建。
- L1：无。

## 6. 协议定稿摘要（决策 D-6~D-9）

完整规范见 **`docs/protocol_v1.md`**（Phase 0 定稿版）。任务书未明确、本次做出决定的要点：

| # | 决策 | 内容与理由 |
|---|---|---|
| D-6 | 端口 | 控制 9100（经典 raw print 端口）、数据 9101，集中于 config_limits.h |
| D-7 | 标称包长算法 | `nominal = ceil(total_bytes/total_packets)`，末包 = 余量；设备回显并逐包强制，彻底消除"非末包长度"歧义 |
| D-8 | BLE UUID | 固定 128-bit 自定义 UUID（protocol_v1.md §2.2），MTU 目标 247，分片 = MTU−3 |
| D-9 | 终态事件统一 | `print_finished` 单事件覆盖 completed/done_with_warning/cancelled/error，另有周期 `print_progress`（200ms/10 包） |
| D-10 | 压缩/校验为任务级 | start_print 必填 `compression`，`integrity` 缺省 none；每包语义由任务级配置决定 |
| D-11 | Wi-Fi 配网 | **开放**：BLE 配网 / SoftAP / 预烧，Phase 5 前请用户选定（不影响 V1 打印协议本身） |
| D-12 | 存储包路径 | `clen==ulen` 的包仍走"压缩缓冲→拷贝入槽"统一路径（少量 memcpy 换取流水线单一性，~64KB 数毫秒级，可接受） |
| D-13 | PSRAM 缺失策略 | 默认拒绝打印任务（`PSRAM_UNAVAILABLE`），不启用受限小缓存模式 |

任务书强制项已原样固化：`#`/`\u0023` 转义、12B Hello 握手、CRC-32/ISO-HDLC（`"123456789"→0xCBF43926`）、LZ4 存储兜底语义（`==`原始 / `<`压缩 / `>`非法）、严格顺序模式、LE 逐字节读取。

## 7. 内存与任务布局摘要

详见 **`docs/memory_budget.md`**。要点：PSRAM 固定占用 512 KiB（6 槽 × 64KB + 2 × 64KB 压缩缓冲），内部 SRAM 静态 ≈16 KB + 应用栈 ≈22 KB，系统 ≈200 KB，余量充足；水位 4/2（6 槽滞回）；吞吐数量级校验全部可行，真实瓶颈在喷头机械/热极限（未知，靠背压适配）。任务：rx@C0/10、decode@C1/12、print@C1/14、ctrl@C0/4、report@C0/3（初值，Phase 6 实测固定）。

## 8. 风险清单

| # | 风险 | 等级 | 缓解 |
|---|---|---|---|
| R1 | LZ4 恶意输入越界/死循环 | 高 | 逐次边界检查 + 无递归 + ASan/UBSan fuzz ≥10 万次（固定种子） |
| R2 | json_lite 解析漏洞 | 高 | 严格模式 + 深度/token/长度三重上限 + fuzz + 截断/溢出专项 |
| R3 | SMP 竞态（槽位/状态机） | 高 | 状态转换单点 + 原子接口 + 主机多线程压测 + 所有权表（memory_budget §4） |
| R4 | BLE 分片重组超时/超限 | 中 | 每连接独立缓冲 + 3s 超时 + 2048 上限 + 20/100/244 切片测试 |
| R5 | APP 忽略 `#` 转义规则 | 中 | 协议文档强制 + 测试向量 + FRAME_TOO_LARGE 防护不扩缓冲 |
| R6 | lwIP 吞吐不足 | 中 | V1 目标仅 500KB/s（默认窗口可达）；Phase 6 实测，必要时调窗口并记录依据 |
| R7 | TWDT 误报（长包解压/发送阻塞） | 中 | decode 分段喂狗；所有等待有限超时 + 可取消 |
| R8 | PSRAM 带宽/Cache 一致性（未来 DMA） | 中 | V1 无 DMA；Phase 7 前先修订 memory_budget.md 再动驱动 |
| R9 | 喷头资料缺失或不可得 | 高（仅影响 Phase 7） | Mock 闭环不受阻；真实打印明确"未实现"，绝不臆造时序 |
| R10 | 令牌/ID 可预测或重放 | 低 | esp_fill_random 硬件 RNG；令牌一次性 + 任务绑定校验 |

## 9. 阶段计划与交付物对照

| Phase | 交付物 | 验收 |
|---|---|---|
| 1 | errors.h/config_limits.h/crc32/frame_splitter/json_lite + host_tests 工程 | 主机编译 + 单测 + ASan/UBSan 通过 |
| 2 | command_schema/command_router/device_info 六命令 | 主机模拟收发；不支持动作返回 UNSUPPORTED |
| 3 | bin_frame_parser/lz4_block/make_packets.py/fuzz | 正常/异常/恶意输入符合预期 |
| 4 | slot_pool/flow_control/job_state_machine/stats | 多线程压测不死锁不覆盖；任意状态可取消 |
| 5 | wifi/tcp×2/ble/psram/task_layout/log/app_main | 设备通道独立运行；idf.py build 零警告 |
| 6 | Mock HAL ×3 + 打印调度 + send_job.py 端到端 | 闭环走通；100 次任务无泄漏 |
| 7 | 真实 HAL（前置：硬件资料 + 风险评估 + 台架方案获用户批准） | 示波器/空载验证后再接喷头 |
| 8 | 完整工程 + 文档 + 依赖审计 | 任务书 12.5 全项 |

## 10. 请用户确认的检查点（进入 Phase 1 的条件）

1. ☐ 协议定稿 `docs/protocol_v1.md`（重点：端口 9100/9101、nominal=ceil 算法、事件模型、D-12 统一路径）
2. ☐ 内存预算 `docs/memory_budget.md`（重点：N16R8 假设、6+2 缓冲、任务优先级初值）
3. ☐ ESP-IDF 版本：是否接受锁定 **v5.3.x LTS**（或告知实际已安装版本）
4. ☐ 板卡型号（U1），以及 Wi-Fi 配网方式偏好（U6/D-11，可 Phase 5 前再定）
5. ☐ 依赖政策确认：`third_party/` 保持为空，无引入申请

## 11. Phase 0 验收清单（对照任务书第十三章）

> **项目进展**（Phase 8 更新）：Phase 0–6 + 8 全部完成；Phase 7 阻塞于喷头官方资料。
> 详见 README 功能状态表 / docs/test_report.md / docs/dependency_audit.md。

- [x] 已知条件、未知条件、默认假设清单（§1/§2 + hardware_questions.md）
- [x] ESP-IDF 选型确认（§3）
- [x] 控制协议与二进制帧格式定稿，含 `#` 转义、数据握手、CRC32、存储兜底（protocol_v1.md）
- [x] 《依赖清单》：third_party/ 为空，无申请表（§5）
- [x] 缓冲区预算（PSRAM/内部 SRAM 分配表）、最大包长、最大任务大小、任务栈与优先级初值及依据（memory_budget.md）
- [x] 风险清单与待确认硬件问题（§8 + hardware_questions.md）
- [ ] **用户确认（§10 检查点）→ 进入 Phase 1**

**按任务书第十五章要求，此处停止，等待确认。**
