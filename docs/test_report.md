# 测试报告（Phase 8 终验）

> 日期：2026-10-15 · 环境：macOS（AppleClang 21 / CMake 4.4）+ ESP-IDF v5.4.1
> 全部主机测试在 `-Wall -Wextra -Werror -fno-exceptions -fno-rtti` + ASan/UBSan 下编译执行；
> 并发模块额外通过 ThreadSanitizer 15 连跑压测（0 告警 / 0 偶发）。

---

## 1. 测试矩阵

| # | 测试 | 断言数 | 覆盖 |
|---|---|---|---|
| 1 | test_crc32 | ~25 | 标准向量（`"123456789"→0xCBF43926`、fox、0..255、协议示例 0x12CB3A3D）；流式=一次性；单比特翻转 |
| 2 | test_frame_splitter | ~45 | 单帧/逐字节/多帧粘包/空帧/恰好上限/超限丢弃/多次 too_large/清除半帧/随机切块（固定种子） |
| 3 | test_json_parse | ~150 | 全类型/整数边界含 INT64_MIN/溢出/前导零/浮点拒绝；转义全集含 `\u0023`/代理对；UTF-8 严格校验（超长/代理/断续/截断 10 种）；重复键/深度 4/5/token 64 上限/字符串 128/129；失败后复用；成员遍历/类型不匹配/数组导航 |
| 4 | test_json_writer | ~226 | 结构/嵌套/逗号正确性；`#`→`\u0023` 强制转义；INT64_MIN 自写整数；容量恰好/不足→整条失败不截断；误用检测（粘滞）×4；根标量/数组根；reset 复用；device_info 量级骨架 |
| 5 | test_command_router | ~192 | 6 命令全路由；信封 3 键/类型/长度校验；schema 严格模式 15 组变异用例（必填/未知字段/类型/范围/枚举/字符集/长度）；start_print 语义交叉校验（乘法一致/包数/字节上限）+ 派生量（nominal/last 含非整除）；能力门控 UNSUPPORTED ×3；hook 错误传播+半写重建；缓冲溢出→INTERNAL_ERROR ×2；msg_id `#` 转义回显 |
| 6 | test_device_info | ~31 | 六节结构确定性子串（键序锁定）；printhead 全 null（禁止编造）；PSRAM 余量上报/null；commands 与 schema 同源（6 条/参数数/ supported 一致）；PSRAM 不可用→start_print supported=false |
| 7 | test_lz4_block | ~120,200 | 手工向量 7 种（纯 literal/游程/offset=2/长度扩展链 284/literal 528/空流）；全位置截断；offset=0/越界/超 expected/容量不足/粘尾；随机 2 万次；变异 10 万次（ASan）；回环 5 图案×16 尺寸逐字节相等 |
| 8 | test_bin_frame_parser | ~230 | Hello 合法/分字节/魔数/版本/令牌/保留字段错；多帧粘包+随机切块+CRC；NeedBuffer 语义（含粘包跨帧头部恰好用尽）；校验矩阵（顺序/长度/CRC/标称/末包/lz4==/clen>ulen）；CRC 单比特翻转 ×2；末包后多余字节；逐字节全流；begin_job 配置校验 |
| 9 | test_slot_pool | ~30,280 | 单线程全语义/FIFO/非法操作拒绝；代次（取消）丢弃+回收+新代次正常；背压非阻塞；**1P1C 3 万项+随机取消**（内容逐字节校验 0 覆盖/槽位 0 泄漏/无死锁）；**2P2C MPMC 4 万项** |
| 10 | test_flow_control | ~22 | 高低水位滞回全行为；构造参数校验；重复 make 重置 |
| 11 | test_job_state_machine | ~40,500 | **表驱动 11 状态 × 12 事件 = 132 组合全覆盖**（25 合法 + 1 幂等 + 全部非法拒绝且状态不变）；每条合法路径实跑 + 终态保持 + 可重启；取消特例（IDLE/终态→NO_ACTIVE_JOB）；200 轮 5 线程竞争（恰 3~4 次转换，无非法状态） |
| 12 | test_stats | ~20 | 计数器原子累加；水位当前/最大 CAS；吞吐窗口（200ms 结算/清零/无新字节→0）；4 线程 4 万次并发 |
| 13 | test_control_plane | ~26 | 3 通道隔离（TCP×2+BLE 分片重组）；一次多条；超长→FRAME_TOO_LARGE；重组超时→FRAME_TIMEOUT+恢复；无 sink 通道安全；init 校验 |
| 14 | test_print_pipeline | ~387 | **端到端闭环**：6 参数组合（1~9 包/跨包行边界/lz4/存储兜底/CRC/单包/4096B）→ 打印内容**逐字节相等**+事件序列+终态统计；接收中途取消→CANCELLED+槽位回收；CRC 损坏→PACKET_CRC_MISMATCH；行回调故障→HAL_FAULT；**连续 100 任务槽位零泄漏** |
| 15 | fuzz_json_splitter | ~177,687 | 拆帧+解析 10 万次变异（固定种子，独立模型验证计数与内容） |
| 16 | fuzz_bin_lz4 | ~200,000 | 帧流 10 万次（改写/截断/拼接合法流+随机切块）+ LZ4 10 万次（ASan 无越界） |
| 17 | py_make_packets_selftest | 50 | Python LZ4 编码器/解码器回环 ×4 图案×12 尺寸；帧流重建；存储兜底验证；**固件解压器交叉验证**（lz4_check 二进制） |

**合计：约 57 万断言/迭代，全部通过。**

## 2. 并发模块验证（任务书 12.3）

| 项 | 方法 | 结果 |
|---|---|---|
| 不覆盖数据 | 逐字节图案校验（1P1C 含随机取消 + 2P2C） | 0 mismatches |
| 不死锁 | 消费者暂停→生产者非阻塞观察饱和→恢复 | 背压路径确认，无死锁 |
| 取消在任意状态 | 表驱动全覆盖 + 流水线中途取消 | CANCELLED + all_slots_free |
| TSan | test_slot_pool + test_print_pipeline 15 连跑 | **0 ThreadSanitizer 告警** |

## 3. 本项目开发过程中修复的缺陷（测试驱动）

| 阶段 | 缺陷 | 捕获方式 |
|---|---|---|
| P1 | Token 布局 24→16 字节 | static_assert |
| P2 | member_key 返回切片非 NUL 结尾 → set_msg 读越界 | 独立调试二进制 |
| P4 | Vyukov 队列 `cells[i].seq` 须初始化为 `i`（写成 0 仅槽 0 可用） | 单线程基础用例 |
| P4 | 测试断言计数器非原子（TSan 报） | TSan |
| P6 | 压缩缓冲按索引序消费→包乱序→数据错乱 | 确定性内容校验（diff 定位到 pkt 边界） |
| P6 | 取消 TOCTOU：print 线程烧掉 CancelDone 一次标志→永卡 Cancelling | DBG 日志 |
| P6 | decode 释放缓冲后才读 ulen→与 rx 写入竞争 | TSan |
| P6 | 致命错误不回收 READY 槽位 | all_slots_free 断言 |
| P6 | decode 提交携带新代次→残留项不被视为过期 | TSan 15 连跑定位 |
| P6 | last_progress_ms_ 跨线程非原子 | TSan |

## 4. 已知限制（诚实声明）

| 项 | 说明 |
|---|---|
| **未在真实 ESP32-S3 上运行** | 全部验证在主机侧完成；`idf.py build` 零告警通过（编译/链接/尺寸 648KB < 3MB），设备上 Wi-Fi/BLE/TCP 通道行为与吞吐/水位需硬件联调（`tools/send_job.py` 就绪） |
| 喷头/走纸/切纸全 Mock | `printer_hal/` 仅含接口 + Mock 实现；Phase 7 需官方硬件资料 |
| 吞吐量与水位实测 | memory_budget §6 数量级校验通过；设备实测数据待硬件接入后补录 |
| 任务栈高水位 | 使用 memory_budget 初值；Phase 6 设备联调时 `uxTaskGetStackHighWaterMark` 实测后固定 |
| BLE MTU 协商 | 依赖中心设备发起；分片按协商后 MTU-3（最低 20） |
| C++ 异常/RTTI | 全程禁用（编译选项 + 代码无 throw/dynamic_cast） |

## 5. 验证命令

```bash
# 主机全量（17 项，~70s）
cmake -S host_tests -B host_tests/build && cmake --build host_tests/build
ctest --test-dir host_tests/build --output-on-failure

# TSan（并发模块，~120s）
cmake -S host_tests -B host_tests/build_tsan -DIJPD_SANITIZER=thread
cmake --build host_tests/build_tsan
ctest --test-dir host_tests/build_tsan -R "slot_pool|state_machine|stats|print_pipeline"

# 设备构建（需 ESP-IDF v5.4.1）
source ~/esp/esp-idf/export.sh && idf.py build
```
