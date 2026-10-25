# ESP32-S3 从零实现 IPP / Apple AirPrint：AI 开发提词与工程方案

> 版本：AirPrint 重启方案 v0.2
> 日期：2026-10-24
> 状态：Phase I 已完成：IPP Core、HTTP transport、capability registry、Get-Printer-Attributes builder、Bonjour TXT/UUID builder、opt-in mDNS wrapper、operation dispatcher、单活动 Job、流式 Print-Job backend sink、Job query/cancel dispatch、ESP32-S3 真机 query + logical Mock Print-Job、真实 image/urf body 黄金样本、公开 Mono8/SW Apple Raster decoder 子集、Print-Job vertical slice、decoder contract、黄金样本 manifest/bundle 工具；默认 app 仍关闭 AirPrint，真实喷头 backend 尚未接入
> 目标工程：`/Users/zzpzero/CLionProjects/ESP32-S3`

## 0. 使用方式

你是一名资深嵌入式网络打印协议工程师、ESP32-S3 / ESP-IDF 架构师、IPP
协议实现工程师和 Apple AirPrint 互操作测试工程师。

本文件是后续 AI 编程任务的唯一 AirPrint 方案入口。每次执行任务前必须：

1. 读取本文件。
2. 检查当前 Git 工作区和目标文件，不假设文件仍与本方案一致。
3. 只实现当前阶段，完成编译、主机测试或设备验证后才能进入下一阶段。
4. 在最终回复中列出修改文件、测试命令、测试结果、未解决风险和下一步。

除非用户明确要求，不得提交 Git commit、不得刷写设备、不得删除用户已有的
无关修改。

## 1. 重要重启规则

这是一次全新的 AirPrint 实现。必须抛弃之前的 IPP/AirPrint 设计、实现、测试
结果和构建变体，不允许通过旧代码恢复功能。

严禁：

- 恢复或重写旧的 `ipp_server.cpp` / `ipp_server.h`。
- 恢复旧的 `pwg_raster.cpp` / `pwg_raster.h`。
- 恢复旧的 `urf_raster.cpp`、旧 Mock Profile 或旧 AirPrint 测试注入。
- 从旧 AirPrint commit cherry-pick、复制实现或依赖旧接口行为。
- 把“曾经能返回 Get-Printer-Attributes”当作 AirPrint 完成标准。
- 在没有真实抓包和黄金样本时猜测 Apple Raster/URF 字段布局。
- 为了让 iPhone 显示打印机而虚报 `pdl`、纸张、色彩、双面、分辨率或状态能力。
- 修改默认 IJPD TCP/BLE/MQTT/RAW 行为来迁就 AirPrint。

允许复用的只有稳定基础能力：

- Wi-Fi STA 连接状态和网络生命周期。
- 当前 mDNS 初始化基础，但 AirPrint TXT 和服务注册必须重新设计。
- `PrintProfile` 的硬件能力数据，但必须经过 AirPrint capability adapter 映射。
- 现有打印 HAL 的真实接口和 Mock 接口。
- 现有打印后端可抽象出的通用 raster sink，但不能直接让 HTTP handler 调用私有状态。

## 2. 当前工程基线

当前工程不是 AirPrint 固件，默认 `main/app_main.cpp` 使用：

```text
Wi-Fi                 enabled
BLE IJPD control     enabled
IJPD TCP control     enabled, port 9100
IJPD TCP data        enabled, port 9101
ESC/POS RAW TCP      disabled by default
MQTT                 disabled by default
IPP/AirPrint         disabled and currently not implemented
Profile              nullptr, fallback IJPD raster capability
Print backend        Mock HAL
ESP-IDF              5.4.1
Target                ESP32-S3
Flash                 16 MB
PSRAM                 8 MB Octal
```

第三方 IJPD 文档位于 `docs/protocols/`。本方案新增的 AirPrint 文档、测试和代码
不得污染 IJPD 控制协议文档；二者是两个独立外部协议入口，但最终共享打印后端。

## 3. 最终目标

在局域网内让以下客户端能够发现并使用打印机：

- macOS 系统打印界面。
- iPhone / iPad 系统打印界面。
- 标准 IPP 工具，例如 `ipptool`、`ippfind`、`dns-sd`。
- 自定义测试客户端和抓包工具。

目标必须同时满足：

1. Bonjour/mDNS 服务记录正确。
2. IPP HTTP 传输和二进制 IPP 编解码正确。
3. `Get-Printer-Attributes` 返回真实能力。
4. `Validate-Job` 对支持和不支持的格式给出正确结果。
5. `Print-Job` 可以流式接收文档，不把整个文档放入 RAM。
6. 打印任务拥有独立的 IPP Job 生命周期。
7. 取消、断开、超时、重复请求和并发请求可控。
8. AirPrint 关闭时，默认 IJPD 固件行为和镜像不受影响。
9. 失败时优先返回标准 IPP 状态，而不是让客户端卡死或伪装成功。

## 4. 非目标

第一版不做：

- IPPS/TLS 和用户认证。
- PDF 解码。
- JPEG/HEIC/PNG 文档解码。
- 多打印任务排队。
- 云端 AirPrint 代理。
- 扫描服务、传真服务、双面打印。
- 通过 AirPrint 复用 IJPD `start_print` JSON 或 TCP `9101` 数据协议。
- 为所有 Apple 设备和所有 macOS 版本提供未经验证的兼容承诺。

第一版只做一个清晰、可验证的垂直切片：

```text
Bonjour discovery
    -> HTTP POST /ipp/print
    -> IPP Get-Printer-Attributes
    -> IPP Validate-Job
    -> IPP Print-Job
    -> one-page image/urf
    -> bounded Apple Raster decoder
    -> common raster print backend
    -> Get-Job-Attributes / Cancel-Job
```

## 5. 方案选型

### 5.1 采用：独立、受限、可测试的 IPP Core + ESP32 Transport

新实现拆成两层：

```text
airprint_core                         airprint_esp
----------------------------------    ----------------------------------
IPP binary codec                      HTTP/1.1 socket server
IPP attribute registry                mDNS/DNS-SD registration
operation/status model                FreeRTOS task and cancellation
job model                             AirPrint <-> common print backend
Apple Raster/URF decoder              profile/capability adapter
host tests and fuzz                   ESP-IDF integration
```

建议目录：

```text
components/airprint_core/
  include/airprint_core/
    ipp_types.h
    ipp_codec.h
    ipp_attributes.h
    ipp_operations.h
    job_model.h
    apple_raster_decoder.h
  src/
    ipp_codec.cpp
    ipp_attributes.cpp
    ipp_operations.cpp
    job_model.cpp
    apple_raster_decoder.cpp

components/airprint_esp/
  include/airprint_esp/
    airprint_service.h
    ipp_http_server.h
    airprint_mdns.h
    airprint_config.h
  src/
    airprint_service.cpp
    ipp_http_server.cpp
    airprint_mdns.cpp
    airprint_backend_bridge.cpp

host_tests/
  test_ipp_codec.cpp
  test_ipp_attributes.cpp
  test_apple_raster_decoder.cpp
  test_airprint_job_model.cpp
```

实际目录名可调整，但必须保持：IPP codec 和 decoder 可在主机上编译，不能依赖
ESP-IDF；HTTP、mDNS、FreeRTOS 和打印后端适配放在 ESP-IDF 层。

### 5.2 不采用 CUPS 全量移植

原因：

- 资源、依赖、线程模型和 POSIX 假设不适合当前固件。
- 第一版只需要有限 IPP 操作和一个确定的 raster PDL。
- 会引入许可证、升级和裁剪边界的不确定性。

### 5.3 不采用不受约束的第三方 IPP 库

除非后续明确完成许可证、内存、ESP-IDF 兼容性和维护状态审计，否则不引入。
当前工程保持 `third_party/` 为空，优先自研有界 codec，并用标准向量和 fuzz
验证。

### 5.4 不把 HTTP handler 当协议层

HTTP 只负责：

- 解析请求行和 header。
- 校验目标路径、Content-Type、Content-Length/Transfer-Encoding。
- 把 body 作为流交给 IPP codec/job worker。
- 把完整 IPP response 返回给客户端。

IPP 属性解析、状态码、Job 生命周期和打印数据解析必须是独立模块。

## 6. IPP over HTTP 方案

### 6.1 初始网络端点

```text
HTTP      TCP 631
Path      /ipp/print
Method    POST
Content-Type: application/ipp
```

第一版只在 Wi-Fi STA 获得 IP 后启动 AirPrint 服务。SoftAP 配网阶段不发布
AirPrint mDNS；是否监听 TCP 631 由配置决定，默认不监听。

### 6.2 HTTP 必须支持

第一版至少支持：

- HTTP/1.1 request line。
- `POST /ipp/print`。
- `Content-Type: application/ipp`。
- `Content-Length`。
- `Connection: keep-alive` 和 `Connection: close`。
- `Expect: 100-continue`，如果客户端发送该 header。
- 有界 header 解析。
- 请求 body 流式读取。
- 客户端断开检测。

兼容性阶段再决定是否需要：

- `Transfer-Encoding: chunked`。
- IPPS/TLS。
- 非标准 URL path 容错。

不能因为实现方便而一次性 `recv()` 假设完整 HTTP 请求，也不能为整个 Print-Job
body 分配一块连续内存。

### 6.3 HTTP 限制建议

所有限制必须由配置集中定义，并在响应失败时关闭连接或返回明确 HTTP/IPP 错误：

| 项目 | 初始建议 |
|---|---:|
| HTTP header 总长度 | 8 KiB |
| 单 header 行 | 1 KiB |
| HTTP 空闲超时 | 15 s |
| IPP 请求属性总长度 | 32 KiB |
| 同时 HTTP 客户端 | 2～4 |
| 同时 Print-Job | 1 |
| Print-Job body | 由 AirPrint Profile 限制，流式处理 |
| HTTP request nesting | 不允许跨请求共享解析状态 |

这些数字必须在第一阶段通过内存预算和设备测试确认，不要散落在代码中。

## 7. IPP 二进制 Codec

### 7.1 编码规则

IPP 二进制字段使用 network byte order，即 big-endian；这与 IJPD 二进制协议的
little-endian 完全不同，必须使用独立的 `read_be16/read_be32/write_be16`。

基本请求结构：

```text
version-number       2 bytes
operation/status     2 bytes
request-id           4 bytes
operation attributes attribute group
job attributes       optional attribute group
document data        optional trailing bytes
```

基本响应结构：

```text
version-number       2 bytes
status-code          2 bytes
request-id           4 bytes
printer attributes   optional
job attributes       optional
document data        optional
```

### 7.2 Parser 必须处理的 value tags

第一版至少实现并严格检查：

- delimiter tags：`operation-attributes-tag`、`job-attributes-tag`、
  `end-of-attributes-tag`。
- integer、enum、boolean。
- textWithoutLanguage、nameWithoutLanguage。
- charset、naturalLanguage、keyword、uri、mimeMediaType。
- resolution（x/y/unit）。
- collection 如果实际 Apple 请求或响应需要；不能因为第一批样本没有就
  永久假设不需要。

每个 attribute 必须验证：

- name length 和 value length 不越界。
- repeated value 的 same-name 规则。
- `name-length=0` 的 additional value 只能延续前一个属性。
- 未知 tag 可以安全跳过，但不能错位。
- 未知属性不应导致整个 TCP task 崩溃。

### 7.3 Codec API 方向

推荐使用显式 reader/writer，而不是构造临时 C 字符串：

```cpp
struct IppAttributeView {
    uint8_t value_tag;
    const char* name;
    size_t name_len;
    const uint8_t* value;
    size_t value_len;
};

class IppReader {
public:
    Err begin(const uint8_t* data, size_t len) noexcept;
    Err next(IppAttributeView* out) noexcept;
    bool at_end_of_attributes() const noexcept;
};

class IppWriter {
public:
    bool operation_attributes_begin() noexcept;
    bool add_keyword(const char* name, const char* value) noexcept;
    bool add_integer(const char* name, int32_t value) noexcept;
    bool add_end() noexcept;
    size_t length() const noexcept;
};
```

实际 API 可以不同，但必须支持固定缓冲、流式输出和错误锁存。

## 8. 第一版 IPP 操作范围

### 8.1 必须实现

| Operation | Code | 第一版用途 |
|---|---:|---|
| `Print-Job` | `0x0002` | 接收一页支持的 raster 文档 |
| `Validate-Job` | `0x0004` | 在不打印的情况下验证属性和格式 |
| `Cancel-Job` | `0x0008` | 取消活动 Job |
| `Get-Job-Attributes` | `0x0009` | 查询 Job 状态和统计 |
| `Get-Jobs` | `0x000A` | 返回当前/最近 Job |
| `Get-Printer-Attributes` | `0x000B` | Apple 发现和能力查询 |

### 8.2 明确返回不支持

对没有实现的 operation 返回 `operation-not-supported`，不能静默返回空成功：

- `Create-Job`
- `Send-Document`
- `Release-Job`
- `Restart-Job`
- `Hold-Job`
- `Resume-Job`
- 扫描相关操作

后续可以增加 `Create-Job + Send-Document`，但第一版优先保证 `Print-Job` 垂直闭环。

### 8.3 Operation 属性

至少解析并验证：

```text
attributes-charset
attributes-natural-language
printer-uri
requesting-user-name     optional
job-name                 optional
document-format          Print-Job / Validate-Job 必须
document-format-version  optional
ipp-attribute-fidelity   optional
requested-attributes     Get-Printer-Attributes optional
job-id                   Cancel/Get-Job-Attributes 必须
```

`document-format` 不在支持列表中时返回
`client-error-document-format-not-supported`，不要先创建一个永远不会完成的 Job。

## 9. AirPrint mDNS / DNS-SD

### 9.1 服务

AirPrint 服务至少需要：

```text
_ipp._tcp
_universal._sub._ipp._tcp
TCP port 631
```

必须在 IPP server 成功监听后再发布；IPP server 停止或 Wi-Fi 断开时撤销服务。

服务实例名和 printer UUID 必须稳定，建议由设备 MAC 派生，不要每次重启随机变化。

### 9.2 TXT 记录

TXT 由 `AirPrintProfile` 生成，禁止散落常量。至少需要根据实际能力生成：

```text
txtvers
qtotal
ty
product
note
adminurl
priority
UUID
rp=ipp/print
pdl
URF
Color
Duplex
Scan
```

只有实现并验证的能力才能加入 TXT：

- 不支持 PDF 就不能在 `pdl` 写 `application/pdf`。
- 不支持彩色就 `Color=F`，不能为了 iOS UI 显示而写 `T`。
- 不支持双面就 `Duplex=F`。
- 不支持扫描就 `Scan=F`。
- `URF` token 必须由实际 decoder/profile 能力生成，不能复制网上样例。

### 9.3 URI 一致性

以下值必须一致：

```text
Bonjour rp       ipp/print
IPP printer URI  ipp://<hostname>.local/ipp/print
HTTP endpoint    http://<ip>:631/ipp/print
```

客户端可能使用 hostname、IPv4 或缓存的 URI 访问；server 应对本机有效 hostname
和端口做合理校验，但不能把任意外部 URI 当作授权依据。

## 10. AirPrint Profile

不要直接把通用 `PrintProfile` 当成完整 AirPrint 能力。新增独立适配结构，例如：

```cpp
struct AirPrintProfile {
    bool enabled = false;
    const char* model = nullptr;
    const char* make_and_model = nullptr;
    const char* printer_name = nullptr;
    const char* location = nullptr;
    const char* media_keyword = nullptr;
    const char* document_format = "image/urf";
    const char* urf_tokens = nullptr;
    uint16_t ipp_port = 631;
    uint16_t dpi_x = 0;
    uint16_t dpi_y = 0;
    uint16_t max_width_px = 0;
    uint32_t max_height_px = 0;
    bool color = false;
    bool duplex = false;
    bool scan = false;
};
```

字段可以不同，但必须分离：

- IPP 属性生成需要的能力。
- mDNS TXT 需要的能力。
- raster decoder 的实际限制。
- common print backend 的硬件限制。

初始化时执行完整 `validate()`：端口、DPI、纸张、格式、URF token、宽高和
backend 能力不一致时拒绝启动 AirPrint，而不是运行后虚报。

## 11. 打印数据格式策略

### 11.1 第一版只支持一种文档格式

推荐第一版只支持：

```text
document-format: image/urf
pages:           1
color:           由 AirPrintProfile 明确选择，初版优先 Mono1/灰度
duplex:          false
```

不支持的格式不要放入 `document-format-supported`、mDNS `pdl` 或 `Validate-Job`
成功响应。

### 11.2 Apple Raster/URF 必须先取样本再实现

实现 decoder 前必须建立黄金样本库：

1. 使用 macOS/iOS AirPrint 流程发送最小黑白测试页。
2. 在受控局域网抓取完整 HTTP/IPP body、HTTP response 和 IPP metadata。
3. 使用 `tools/airprint_sample.py import` 把原始 evidence 复制到独立 bundle，
   再用 `check --bundle` 校验每个 artifact 的长度和 SHA-256。
4. 人工记录 `document-format`、document offset/length、页尺寸和预期像素；工具
   不从抓包内容推断这些字段。
5. 对不同纸张、DPI、方向、边距、空白页和取消流程重复抓取。
6. 用独立 Python 工具解析和可视化样本，但不得把解析猜测写回 manifest。
7. 只有确认字段含义后，才编写 ESP32 decoder。

不能根据文件名、网上不完整结构体或旧 AirPrint 代码猜测 header。

### 11.3 Decoder 约束

decoder 必须：

- 支持任意 TCP/HTTP body 分块。
- 不依赖整个文档常驻内存。
- 对所有 header、宽高、stride、页数、压缩长度做上限检查。
- 对压缩输入和解码输出分别做上限检查；不能只限制 compressed document bytes 而允许
  重复行展开成无界输出。
- 将每一行通过 `RasterSink` 交给后端。
- 在行级别支持取消、超时和 backend backpressure；当前 host `RasterSink` 行回调是同步的，
  行指针只在 callback 期间有效，异步 backend 必须复制。
- 对截断、伪造大尺寸、整数溢出、错误 stride、未知版本返回明确错误。
- 输出统一的 `RasterPageDescriptor`，不把 Apple Raster 字段泄漏到 HAL。

## 12. IPP Job 与打印后端

IPP job 不能直接调用当前 `PrintService` 的私有变量。新增明确的共享后端接口：

```cpp
struct RasterPageDescriptor {
    uint32_t width_px;
    uint32_t height_px;
    uint32_t bytes_per_row;
    uint16_t dpi_x;
    uint16_t dpi_y;
    PixelFormat pixel_format;
    bool last_page;
};

class RasterSink {
public:
    virtual Err begin_page(const RasterPageDescriptor&) noexcept = 0;
    virtual Err write_row(const uint8_t*, size_t, uint32_t row) noexcept = 0;
    virtual Err end_page(Err status) noexcept = 0;
    virtual Err cancel() noexcept = 0;
};
```

现有 IJPD raster pipeline 和 AirPrint decoder 都实现 producer adapter，最终进入
同一个受控 backend。不要让 AirPrint 代码伪造 IJPD `start_print`、token 或 TCP
数据帧，也不要让 HTTP task 直接操作 MockPrinthead。

第一版 Job manager 建议只允许一个活动 Job：

```text
pending -> processing -> completed
                    -> aborted
pending -> canceled
```

记录：

- job-id、job-uri、job-uuid。
- job-name、requesting-user-name。
- document-format、pages-completed、job-k-octets。
- printer-state、job-state-reasons。
- create/start/completion time。
- cancel/disconnect/decoder/backend error。

## 13. 并发模型

建议任务边界：

```text
airprint_http_task       接收连接、解析 HTTP、发送 IPP response
airprint_job_task        解析 IPP operation、管理 Job 状态
airprint_decode_task     流式 Apple Raster decode
existing_print_task      common backend/HAL
```

原则：

- HTTP task 不执行长时间打印。
- IPP codec 不调用 FreeRTOS API。
- decoder 和 HTTP body reader 之间使用有界 ring/queue。
- producer 阻塞必须有超时并可被 Cancel-Job/断开唤醒。
- 只有 job owner 能改变该 Job 的终态。
- Job 终态后释放 body buffer、decoder state 和 backend reservation。
- 不允许 AirPrint HTTP task 和 IJPD task 同时占用唯一打印 backend。

## 14. 内存和资源预算

AirPrint 开关打开后新增预算必须单独列出：

| 资源 | 初始策略 |
|---|---|
| HTTP header buffer | 固定 8 KiB |
| IPP attribute buffer | 固定有界，建议 32 KiB |
| decoder scratch | 由 URF 行宽和格式决定，PSRAM 优先 |
| body ring | 2～4 个固定块，不保存整个文档 |
| HTTP client | 2～4 个上限 |
| active AirPrint jobs | 1 |
| mDNS TXT | 固定 Profile 生成，限制长度 |
| IPP response | 固定写出 buffer，溢出返回内部错误 |

必须对比：

- default build 与 AirPrint build 的 Flash 增量。
- 内部 RAM/DIRAM 增量。
- PSRAM 保留量。
- 所有新增 task 的 stack high-water mark。
- Wi-Fi TCP 吞吐和 IJPD 延迟回归。

## 15. 新增配置和默认行为

建议采用编译期和运行时双门控：

```text
AIRPRINT_ENABLE=OFF             default CMake build
PrinterServices.airprint=false  default runtime configuration
```

AirPrint Core 的显式构建使用：

```bash
tools/idf.sh -B build-airprint build -DAIRPRINT_ENABLE=ON
```

在它接入 `airprint_esp` 之前，`AIRPRINT_ENABLE=ON` 只用于编译和测试
platform-independent AirPrint Core，不会自动启动 HTTP、mDNS 或打印服务。

启用时才编译 `airprint_core` / `airprint_esp`；运行时还必须有有效
`AirPrintProfile`。三种状态必须可区分：

1. 未编译：固件没有 AirPrint 符号、端口和 mDNS。
2. 已编译但关闭：没有监听、没有广播、`get_device_info` 报告 false。
3. 已编译且启用：只有 IPP server 成功监听后才广播 `_ipp._tcp`。

默认 IJPD build 的行为必须保持：

- TCP `9100/9101` 不变。
- BLE UUID 和控制 framing 不变。
- mDNS 只有 `_ijpd._tcp`，不能出现 `_ipp._tcp`。
- 不增加默认启动 task。
- 不改变默认打印结果和内存预算。

## 16. 分阶段实施计划

### Phase A：审计和协议冻结

交付：

- AirPrint capability matrix。
- HTTP/IPP operation matrix。
- Apple Raster 样本采集脚本和样本格式。
- 资源预算和错误码映射。
- 不修改默认固件行为。

退出条件：用户确认第一版只支持的 document-format、颜色、纸张、DPI、单页策略。

### Phase B：IPP Core

实现：

- big-endian reader/writer。
- attribute parser/writer。
- operation/status enums。
- bounded collection support。
- request validation。

测试：

- 每个 value tag 的合法/非法向量。
- 截断、超长、重复名、unknown tag、attribute continuation。
- fuzz 不崩溃、不越界、不无限循环。

### Phase C：HTTP Transport

实现：

- 独立 TCP `631` server。
- `/ipp/print` 路由。
- keep-alive、Content-Length、Expect、断开和超时。
- bounded clients/body streaming。

测试：

- `curl` 错误请求。
- partial header/body、粘包、慢速客户端、重复请求。
- server stop 时连接收敛。

### Phase D：Printer Attributes + mDNS

实现：

- `Get-Printer-Attributes`。
- capability registry。
- `_ipp._tcp` canonical AirPrint service + same-instance `_universal` subtype。
- profile-driven TXT。

测试：

- `dns-sd` / `ippfind`。
- mDNS service disappear/reappear。
- macOS `ipptool get-printer-attributes`。

此阶段的 query-only 行为已作为默认/无 printhead 配置保留；显式 Mock variant
已继续完成受限 `Print-Job` 垂直闭环，避免把 logical Mock 宣称成真实喷头打印。

### Phase E：Validate-Job + URF Decoder

实现：

- 一页 `image/urf` parser/decoder。
- 真实抓包黄金样本。
- `Validate-Job` 和 document format rejection。

测试：

- 每个黄金样本的 page descriptor、行数、stride、像素 CRC。
- 截断、超宽、超高、错误版本、错误 stride、错误颜色格式。

### Phase F：Print-Job 垂直闭环

实现：

- single active Job manager。
- decoder -> RasterSink -> common backend。
- `Print-Job`、`Get-Job-Attributes`、`Get-Jobs`。
- `print-job` 完成/错误事件。

验收：

- macOS 一页黑白测试页真实打印到 Mock backend。
- 端到端输入输出 CRC 一致。
- HTTP client 断开后 Job 不永久卡住。

### Phase G：Cancel / Apple Compatibility

实现：

- `Cancel-Job`。
- iOS/iPadOS/macOS capability query 顺序兼容。
- 多个 Get-Printer-Attributes 并发请求。
- 设备重启/网络断开后的 Job 清理。

验收：

- macOS 打印对话框发现、预览、发送、取消。
- iPhone/iPad 系统打印界面发现、发送、取消。
- 无 Print-Job 时只产生查询，无死锁和资源泄漏。

### Phase H：稳定性和发布

- 默认 build 与 AirPrint build 双构建。
- Host ASan/UBSan/fuzz。
- 设备长时间 mDNS/HTTP 查询压力。
- 多客户端、断网、重连、重复 Print-Job。
- 内存、栈、TCP 吞吐、打印后端回归。
- README 和 `docs/protocols/` 更新。

## 17. 测试工具和验收命令

主机基础测试：

```bash
cmake -S host_tests -B host_tests/build
cmake --build host_tests/build
ctest --test-dir host_tests/build --output-on-failure
```

当前 AirPrint Core/query/Print-Job 测试包括：

```bash
test_ipp_codec
fuzz_ipp_codec
fuzz_apple_raster
test_ipp_probe
test_airprint_sample
py_airprint_query_selftest
py_airprint_urf_analyze_selftest
```

真实 Apple Raster/URF 样本已经采集并完成公开 CUPS framing 分析；platform-independent
Mono8/SW decoder 子集和 Print-Job -> RasterSink vertical slice 已通过 golden，并完成
host-only callback、multi-page 和错误传播边界测试。其他颜色/位深、保留字段、真实
硬件 backend pixel contract，以及 ESP32 decoder 的设备侧 fuzz target 仍需独立确认。

真机 query probe 刷写、配网、mDNS、抓包和 evidence 保存步骤见
`docs/airprint_probe_runbook.md`。

标准工具：

```bash
dns-sd -B _ipp._tcp local
ippfind --ls
ipptool -tv ipp://<host>.local:631/ipp/print get-printer-attributes.test
```

设备构建：

```bash
tools/idf.sh -B build-default build
tools/idf.sh -B build-airprint build
```

设备验证必须保存：

- mDNS 浏览输出。
- IPP request/response 十六进制样本。
- HTTP header/body 边界样本。
- Apple Raster 原始输入和解码后行 CRC。
- Job 状态时间线。
- heap 和 task stack high-water mark。

## 18. 完成标准

AirPrint 功能只有全部满足以下条件才可标记为完成：

- `Get-Printer-Attributes`、`Validate-Job`、`Print-Job`、Job 查询和取消均有测试。
- macOS 和至少一台 iOS/iPadOS 设备完成发现与一页真实流程。
- mDNS TXT 与 IPP 属性一致，未声明不支持能力。
- 所有输入均有长度、类型、状态和超时边界。
- Print-Job body 不整份驻留 RAM。
- HTTP 断开、Cancel-Job、重启、断网均能释放资源。
- AirPrint 关闭时默认 IJPD 主机测试和设备启动行为不回归；默认构建不包含
  `airprint_core`、`airprint_esp`，也不监听 TCP 631 或发布 IPP mDNS。
- 没有恢复旧 AirPrint 文件、旧接口、旧测试或旧 build variant。
