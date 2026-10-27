# 测试报告

> 当前记录：2026-10-24 · 环境：macOS + ESP-IDF v5.4.1
> 主机测试在 `-Wall -Wextra -Werror -fno-exceptions -fno-rtti` + ASan/UBSan 下编译执行；
> 并发模块额外通过 ThreadSanitizer 15 连跑压测（0 告警 / 0 偶发）。

当前验证结果：主机 `40/40` 通过；ESP-IDF 默认镜像构建通过；默认镜像已刷写
到 ESP32-S3 并成功启动。构建仍有 `provisioning_portal.cpp` 的既有警告和
volatile counter 警告，不能再表述为“零告警”。

本轮全量测试中，Ricoh MH5440 logical Mock、HAL bundle 注入接口和 AirPrint 测试均稳定通过。

`AIRPRINT_ENABLE=ON` 的显式 AirPrint Core + HTTP/query/Print-Job transport 构建也已
通过，最新镜像为 `0x12fad0` bytes（约 `1.20 MiB`）；默认构建排除 `airprint_core` 和 `airprint_esp`，当前
没有 TCP 631 listener 或 mDNS IPP 注册。

本轮已完成一次 ESP32-S3 真机 AirPrint query 联调：设备 MAC `10:20:ba:71:d5:d0`，
连接 `Piao` 后获得 `172.20.10.3`，`_ijpd._tcp` 和 TCP `631` 均启动；原始
`Get-Printer-Attributes` 请求返回 HTTP `200`、IPP `successful-ok`。完整证据保存在
`evidence/query-001/`，包括 HTTP/IPP request/response 和 SHA-256 metadata。此次只
验证了原始查询链路；CUPS `ipptool -tv ... get-printer-attributes.test` 也已通过。
随后已从 macOS Photos/PrintKit 捕获完整的系统查询和 `Print-Job` 流，证据见下方
两个 pcap 和 golden bundles。

`evidence/system-query/airprint-query.pcap` 和
`evidence/system-query-002/airprint-printjob.pcap` 已捕获 macOS Photos/PrintKit 的
真实查询、`Print-Job` metadata 和 chunked HTTP transport，包括
`document-format=image/urf`、`application/octet-stream` 变体。第二轮 pcap 已成功
重组出 5 个完整 chunked `Print-Job` 流，得到两组重复的 `image/urf` document body：
`707738` 字节和 `722531` 字节。原始 pcap、IPP body、document bytes、HTTP request
和 metadata 已导入 `evidence/golden-samples/`，每组 bundle 校验通过。

公开 CUPS Apple Raster framing 分析已完成：两组 document 均为 `UNIRAST\0`、1 page、
`850x1100`、100 DPI、8 bits/pixel、SW 单色、850 bytes/row，1100 行均消费到 EOF；
row CRC32 分别为 `9b3ac4d5` 和 `ea4be99e`。分析结果保存在各黄金 bundle 的
`analysis.json` 和 `evidence/golden-samples/urf-analysis-compare.json`。这确认了
公开 framing 与 row encoding，不代表保留字段或设备后端像素语义已经确认。

`AppleRasterDecoder` 已实现并通过两组真实 golden：公开 `UNIRAST\0` framing、32-byte
page header、Mono8/SW、modified PackBits、增量 feed、row sink、CRC、截断、尺寸/行
repeat 上限和取消路径。新增 host 边界覆盖了 clear-to-end、literal/repeat 极值、
多页 `last_page`、保留字段透传、callback 顺序、sink 失败和失败后原始 decoder
错误保留，以及压缩输入和解码输出的独立资源上限。默认 decoded-output 上限为
`256 MiB`，调用方可通过 `RasterDecoderLimits::max_decoded_bytes` 收紧；超限页在
`begin_page` 前拒绝。`RasterSink::write_row` 的行指针只在同步 callback 期间有效，
后端如需异步使用必须复制。该 decoder 仍只存在于 platform-independent Core/host
tests，没有接入默认 app 或硬件 backend。

`AppleRasterPrintBackend` 已把 `IppPrintJobStream` 的 metadata/document chunks 接到
该 decoder 和 `RasterSink`；两组真实 golden 的完整 IPP body vertical slice 均通过。
host bridge 现在按 decoder 完成页数填充 IPP `pages-completed`，并在 malformed raster
后保留原始 decoder 错误，同时将 Job 标记为 `aborted`。`document-format` 在 stream
入口按 ASCII 不区分大小写匹配；裸 decoder 允许调用方处理 `consumed < length` 的
尾部，而 Print-Job bridge 会拒绝 raster 文档后的 trailing bytes。该 adapter 只用于
host/opt-in Core 验证，不会调用 ESP32 printer HAL。

`MockRicohMh5440Hal` 已接入默认 `main/app_main.cpp` Profile 和 `PrinterHalBundle`；
默认 ESP32 镜像运行 RICOH MH5440 logical CMYK8 Mock。该 Ricoh Mock 只验证 CMYK8
逻辑行、通道统计/CRC、确定性故障和生命周期，不代表真实喷头电气驱动；真实喷头
`printhead.is_configured` 仍保持 `false`，直到厂商电气资料和真实控制器完成闭合。

显式 `AIRPRINT_ENABLE=ON` 镜像已在同一 ESP32-S3 上开启 AirPrint Print-Job，并通过
TCP `631` 接收第一组真实 golden `ipp_body.bin`（`707738` bytes）。HTTP 返回 `200`，
IPP 返回 `successful-ok`，Job `1` 为 completed，`pages-completed=1`、
`job-k-octets=692`。服务同时通过 `_ipp._tcp.local` 发布
`RICOH MH5440 Mock`。完整摘要见 `evidence/real-ricoh-mock-001/airprint-printjob-001.md`。
当前是逻辑 Mono8 -> CMYK8 Mock conversion path，不代表真实喷头点火。

本轮首次发送照片时暴露并修复了 ESP HTTP body 同 chunk metadata/document 的 consumed
处理问题；修复后真实 Print-Job 已稳定返回。AirPrint opt-in runtime evidence 见
`evidence/real-ricoh-mock-001/airprint-optin-runtime.md`。

随后补齐了 iOS/PrintKit 可能使用的 Job selector 路径：`Get-Job-Attributes` 现在
接受并校验完整 `job-uri` 或 `job-id`，而 parser 不再要求 Job 查询带
`printer-uri`。最新镜像在真机上以 Job `1`/`2` 验证：`Get-Jobs` 返回 completed，
精确 `job-uri` 和 `job-id=1` 均返回 HTTP `200`、IPP `successful-ok` 和 Job
attributes，错误 URI 返回 `client-error-not-found`。系统 CUPS 自带的
`get-job-attributes.test` 默认把 printer URI 当作 job URI，因而不是有效的 Job
selector 互操作测试；使用真实 `.../job/<id>` URI 的原始 IPP 请求已通过。

本轮 iOS 兼容性修复还移除了重复 AirPrint subtype 广播，并把 Apple Raster 解码器
从仅 `SW/Mono8` 扩展到 8-bit `sRGB/RGB/AdobeRGB/CMYK`。此前服务错误地广告
`Color=T`，但彩色照片页面会在 decoder 页头被拒绝，导致 Job aborted 和 iOS 的
“printer is no longer available”。修复后的 ESP32 真机已通过 RGB8、CMYK8 合成
任务和真实照片 golden；`Validate-Job`、`Get-Job-Attributes` 也均返回成功。详细
证据见 `evidence/real-ricoh-mock-001/airprint-ios-compatibility-001.md`。

现有系统 Print-Job 抓包还显示 metadata 同时声明 `image/urf` 和
`application/octet-stream`。解析器现在允许同一请求中的重复 `document-format`
声明，并把 octet-stream 按 URF auto-sensing 处理，覆盖 iPhone 可能采用的格式
声明顺序。

随后在最终诊断镜像上确认 iPhone 已成功提交：Job `2`，名称 `照片`，用户
`mobile`，`image/urf`，状态 `completed`，`pages-completed=1`，
`job-k-octets=2415`；对应 `Get-Job-Attributes` 返回 HTTP `200` / IPP
`successful-ok`。当前固件是逻辑 `MockRicohMh5440Hal`，因此没有真实纸张输出是预期的。

本轮已完成默认 Ricoh Mock ESP32-S3 真机回归，完整摘要见
`evidence/real-ricoh-mock-001/README.md`。设备 MAC 为
`10:20:ba:71:d5:d0`，通过 `Xiaomi_D551` 获取 `192.168.31.100`。验证覆盖
`get_device_info`、显式 CMYK8 五行任务、显式四平面 channel order、取消、CRC 错误、
数据连接断开、数据连接超时、超时后恢复任务、`feed_paper` 和预期的
`cut_paper=UNSUPPORTED`。超时事件最终返回 `JOB_TIMEOUT`。

默认 App 预览已按 Ricoh A4 logical Mock 修正：CMYK8 interleaved 输入使用
`2480×3508 px @300 DPI`、`9920 bytes/row` 和 `210 mm` A4 逻辑纸张；超过完整页缓存
阈值时使用 preview-only 实时渲染。A4 模型不代表真实 MH5440 喷头覆盖范围。

AirPrint 显示生命周期已补齐：Apple Raster 每页建立独立的显示任务，逐行预览更新与
CMYK8 Mock HAL 的 `submitRaster` 行回调保持同一同步边界；完成、取消、HTTP 中断、HAL
失败和等待 HAL 空闲失败均幂等结束显示任务，LCD 初始化不可用时打印继续降级运行。
该补丁已通过本轮 `40/40` host tests、默认 ESP-IDF 构建和 `AIRPRINT_ENABLE=ON`
构建；最新刷机后的 LCD 逐行刷新和无 reset/panic 仍待设备验证。

BLE legacy advertising payload 的 `adv fields rc=4` 已修复：flags + 128-bit Service UUID
保留在主广播包，完整 `IJPD-Printer` 名称移动到 scan response，避免超过 31-byte
广播上限。默认固件和 AirPrint probe 均构建通过；最新 AirPrint probe 已刷入
`/dev/cu.usbmodem101`，启动日志确认 `ble_ctl: advertising as IJPD-Printer`，且未再
出现 `adv fields rc=4`。BLE 广播启动已验证，具体客户端扫描、连接和 GATT 控制回环仍待
手机或 BLE 测试客户端验证。

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
| 9 | test_mqtt_chunk_protocol | ~15 | IJCH 头部/长度/魔数/CRC 校验；顺序接收与乱序拒绝 |
| 10 | test_slot_pool | ~30,280 | 单线程全语义/FIFO/非法操作拒绝；代次（取消）丢弃+回收+新代次正常；背压非阻塞；**1P1C 3 万项+随机取消**（内容逐字节校验 0 覆盖/槽位 0 泄漏/无死锁）；**2P2C MPMC 4 万项** |
| 11 | test_flow_control | ~22 | 高低水位滞回全行为；构造参数校验；重复 make 重置 |
| 12 | test_job_state_machine | ~40,500 | **表驱动 11 状态 × 12 事件 = 132 组合全覆盖**（25 合法 + 1 幂等 + 全部非法拒绝且状态不变）；每条合法路径实跑 + 终态保持 + 可重启；取消特例（IDLE/终态→NO_ACTIVE_JOB）；200 轮 5 线程竞争（恰 3~4 次转换，无非法状态） |
| 13 | test_stats | ~20 | 计数器原子累加；水位当前/最大 CAS；吞吐窗口（200ms 结算/清零/无新字节→0）；4 线程 4 万次并发 |
| 14 | test_control_plane | ~26 | 3 通道隔离（TCP×2+BLE 分片重组）；一次多条；超长→FRAME_TOO_LARGE；重组超时→FRAME_TIMEOUT+恢复；无 sink 通道安全；init 校验 |
| 15 | test_print_pipeline | ~387 | **端到端闭环**：6 参数组合（1~9 包/跨包行边界/lz4/存储兜底/CRC/单包/4096B）→ 打印内容**逐字节相等**+事件序列+终态统计；接收中途取消→CANCELLED+槽位回收；CRC 损坏→PACKET_CRC_MISMATCH；行回调故障→HAL_FAULT；**连续 100 任务槽位零泄漏** |
| 16 | fuzz_json_splitter | ~177,687 | 拆帧+解析 10 万次变异（固定种子，独立模型验证计数与内容） |
| 17 | test_middleware | ~100 | 热敏/喷墨 Profile 校验、ESC/POS 流水线逐字节转发、能力上报 |
| 18 | test_ricoh_mh5440_mock_hal | CMYK8 行校验、逐通道统计/CRC、固定缺喷嘴、节拍、行/字节故障、backpressure、纸路/过温故障、stop 幂等 |
| 19 | test_ricoh_mh5440_pipeline | PrintPipeline 三泵 -> CMYK8 Mock 行提交、通道 CRC/统计、正常完成、HAL_FAULT 终态和槽位回收 |
| 20 | fuzz_bin_lz4 | ~200,000 | 帧流 10 万次（改写/截断/拼接合法流+随机切块）+ LZ4 10 万次（ASan 无越界） |
| 21 | test_ipp_codec | IPP big-endian header、attribute groups、重复属性值、document body、响应写出、截断和输出容量边界 |
| 22 | test_ipp_request | IPP operation/group 顺序、必需属性、类型、重复属性、document-format、Job selector 和 document body 验证 |
| 23 | test_http_request_parser | HTTP/1.1 partial header、粘包 body、Content-Length、chunked body、Expect、keep-alive、冲突和大小限制 |
| 24 | test_airprint_capabilities | AirPrint capability validation、requested-attributes 过滤、Get-Printer-Attributes response round-trip、MIME/keyword/collection/URI tag 和容量错误 |
| 25 | test_airprint_mdns | AirPrint UUID version/variant、TXT capability 映射、稳定性和无效能力拒绝 |
| 26 | test_ipp_operations | Get-Printer-Attributes、unsupported operation、bad request、Job 查询/取消和 request-id 保持 |
| 27 | test_ipp_probe | IPP body 分块、Get-Printer-Attributes 成功、无 JobStore 时 unsupported operation、malformed request、请求上限 |
| 28 | test_ipp_job | 单活动 Job 创建、重复创建、processing/completed/canceled/aborted、幂等取消和资源重置 |
| 29 | test_ipp_print_job | Print-Job metadata/body 分片、backend sink、完成、大小写 MIME、取消、取消失败、空文档、超限和 backend failure |
| 30 | test_airprint_sample | decoder contract、样本格式、document range、SHA-256 和 known expected values 边界 |
| 31 | test_apple_raster_decoder | 合成 Apple Raster 子集、增量 feed、clear/literal/repeat 边界、保留字段、多页、压缩/解码 byte limit、sink failure、cancel |
| 32 | test_apple_raster_print_job | 合成和真实 IPP Print-Job metadata、逐行 sink、两页统计、callback 顺序、malformed/trailing raster abort 和 decoder error 保留 |
| 33 | fuzz_ipp_codec | 10 万次确定性随机 IPP 输入，验证 reader 不越界、不崩溃、不无限循环 |
| 34 | fuzz_apple_raster | 5 万次确定性 Apple Raster 合法/截断/变异输入，增量切块、consumed/state、sink contract 和无死循环 |
| 35 | test_apple_raster_decoder_golden | 两组真实 `image/urf` document 的 descriptor、1100 行输出和 row CRC32 |
| 36 | test_apple_raster_print_job_golden | 两组真实 IPP body 经 Print-Job stream 到 Apple Raster sink 的完整 vertical slice |
| 37 | py_make_packets_selftest | Python LZ4 编码器/解码器回环、帧流重建和固件解压器交叉验证 |
| 38 | py_airprint_sample_selftest | 黄金样本 manifest create/check、bundle artifact 导入、逐 artifact SHA-256 完整性和篡改/非法范围拒绝 |
| 39 | py_airprint_query_selftest | 原始 IPP query 生成、HTTP/IPP response 采集、完整 evidence 文件和 metadata SHA-256 |
| 40 | py_airprint_urf_analyze_selftest | 公开 Apple Raster framing、32-byte page header、modified PackBits 行统计和跨样本差异 |

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
| AirPrint probe | HTTP task 栈中放置 8 KiB parser + socket buffer 导致真实查询连接 reset | ESP32-S3 真机 query；将有界 parser/buffer 移出 task stack |

## 4. 已知限制（诚实声明）

| 项 | 说明 |
|---|---|
| **尚未完成真实打印链路验证** | 默认 Mock 固件已在 ESP32-S3 上启动并确认 Wi-Fi 配网门户、TCP 9100/9101、BLE 服务；真实喷头、RAW 打印、吞吐/水位和完整打印结果仍需硬件联调 |
| 喷头/走纸/切纸全 Mock | `printer_hal/` 仅含接口 + Mock 实现；Phase 7 需官方硬件资料 |
| 吞吐量与水位实测 | memory_budget §6 数量级校验通过；设备实测吞吐和水位待硬件接入后补录 |
| 任务栈高水位 | 使用 memory_budget 初值；Phase 6 设备联调时 `uxTaskGetStackHighWaterMark` 实测后固定 |
| BLE MTU 协商 | 依赖中心设备发起；分片按协商后 MTU-3（最低 20） |
| C++ 异常/RTTI | 全程禁用（编译选项 + 代码无 throw/dynamic_cast） |

## 5. 验证命令

```bash
# 主机全量（40 项，约 90s）
cmake -S host_tests -B host_tests/build && cmake --build host_tests/build
ctest --test-dir host_tests/build --output-on-failure

# TSan（并发模块，~120s）
cmake -S host_tests -B host_tests/build_tsan -DIJPD_SANITIZER=thread
cmake --build host_tests/build_tsan
ctest --test-dir host_tests/build_tsan -R "slot_pool|state_machine|stats|print_pipeline"

# 设备构建（需 ESP-IDF v5.4.1）
source ~/esp/esp-idf/export.sh && idf.py build
```
