# ESP32-S3 DIY 6 英寸彩色喷墨照片打印机固件开发任务书（V1.0 自研优先版）

> 版本：V1.0（全新项目，从零开发）
> 适用范围：ESP32-S3 固件软件开发
> 目标平台：**ESP-IDF（首选，原生 CMake 构建）**
> 语言：C++17（核心层可限制为 C++ 的“类 C”子集，不依赖异常与 RTTI）
> 文档用途：交给 AI 编程助手，按阶段生成可编译、可验证、可自主维护的固件工程
>
> **核心原则（优先级最高）：自研优先、依赖最小化。**
> 除 ESP-IDF 自带组件外，默认**不引入任何第三方库**。协议解析、JSON 子集、LZ4 解压、CRC、环形缓冲、状态机、任务调度、HAL 全部自研。仅当第三方库同时满足“质量极高、免费开源、许可证宽松、可审计、体积小”并通过第 2 章的准入标准与《依赖申请表》审批时才允许引入。
>
> **重要边界：** 本任务书只规定通信、数据接收、缓存、任务调度和硬件抽象接口。具体喷头的驱动时序、电压、点火波形、喷嘴映射、墨水系统要求，必须依据实际喷头型号和官方资料实现；不得臆造。A4988 只用于兼容的步进电机驱动场景，不能视为喷头驱动器。

---

## 一、角色与总目标

你是一名资深嵌入式系统架构师，熟悉 ESP32-S3、ESP-IDF、FreeRTOS（含 SMP 双核）、lwIP、NimBLE（ESP-IDF 内置）、二进制协议设计、PSRAM/DMA 内存约束、流式数据处理及嵌入式可靠性设计。

请基于下述协议和工程约束，构建一套**自主可控**、结构清晰、可编译、可在主机上单元测试、可逐步接入真实硬件的固件工程。系统应支持：

1. BLE 与 Wi-Fi TCP 控制通道。
2. 独立的 Wi-Fi TCP 二进制数据通道。
3. `#` 作为控制 JSON 报文结束符。
4. 打印任务创建、数据分包接收、校验、解压、排队、打印调度及完成状态回报。
5. PSRAM 有界环形缓冲、背压流控、超时与错误恢复。
6. 喷头与走纸电机的硬件抽象层（HAL），在硬件参数未确认前提供安全的模拟实现，不得假装真实喷头已经可以点火打印。

**交付标准：** 不只给片段或伪代码；按阶段输出完整文件、构建配置、构建步骤、测试方法和验收结果。每个阶段先通过编译与测试，再进入下一阶段。

---

## 二、自研优先与依赖政策

### 2.1 依赖分级

| 级别 | 含义 | 规则 |
|---|---|---|
| L0 | 工具链与 ESP-IDF 自带组件：FreeRTOS、lwIP、esp_wifi、NimBLE（IDF 内置）、esp_psram、heap_caps、driver（GPTimer / SPI / RMT / LCD_CAM / MCPWM / PCNT 等）、esp_timer、NVS、Unity（IDF 自带测试框架） | 允许直接使用，并**锁定 IDF 版本**（选一个 v5.x 稳定版，记录在 README） |
| L1 | 第三方库 | **默认禁止**，须通过 2.3 准入审批 |
| L2 | 仅主机侧开发工具（测试向量生成、对照验证） | 允许使用 Python 标准库；禁止在固件产物中引入 |

### 2.2 必须自研清单

以下模块一律自研，AI 不得用第三方库替代：

| 模块 | 自研要求 |
|---|---|
| 控制帧拆分器 | 以 `#` 为界的有界流式拆帧，零动态分配 |
| JSON 子集解析器 | 就地（in-place）分词，固定 schema，限制深度与长度，零堆分配（见 5.3） |
| JSON 写出器 | 带转义与容量检查的流式写入器 |
| 二进制帧状态机 | 支持半包/粘包，见第六章 |
| LZ4 Block 解压器 | 按公开的 LZ4 Block 格式自行实现，见 6.4 |
| CRC32（可选 CRC16） | 自写查表或逐位实现，附标准测试向量 |
| 环形缓冲 / 槽位池 | 固定容量，带状态与代次标识 |
| 打印状态机、流控、统计 | 集中实现 |
| HAL 抽象与 Mock | 全部自写 |
| 步进电机控制（梯形加减速） | 基于 GPTimer/MCPWM 自写，不引入步进电机库 |

### 2.3 第三方库准入标准（逐条满足才可引入）

1. **许可证**：MIT / BSD / Apache-2.0 / zlib 等宽松协议，且允许闭源商用。禁止 GPL / LGPL / 强 Copyleft 与来路不明代码。
2. **质量**：公认的业界标杆、维护活跃、有测试与较多真实产品使用记录。
3. **体积与内存**：Flash/RAM 占用可量化，不得带来隐式动态分配或不可控内存增长。
4. **可审计**：源码规模可控，可锁定到具体 commit/版本并 vendor 到仓库（`third_party/`，附 LICENSE 与来源记录），**不得**在线拉取 `latest`。
5. **可替换**：通过项目内自有接口封装，任何时候可替换为自研实现。
6. **必要性**：自研成本显著高于收益，或自研存在明显安全/正确性风险。

引入前必须提交《依赖申请表》，格式如下，并等待用户确认：

```text
库名 / 版本 / 提交哈希：
许可证：
用途与自研替代方案对比：
Flash 与 RAM 占用估算：
是否含动态分配 / 是否线程安全：
封装接口位置：
替换成本：
结论（建议引入 / 建议自研）：
```

### 2.4 本项目的默认依赖决策

| 候选 | 决策 | 理由 |
|---|---|---|
| Arduino-ESP32 / PlatformIO 封装层 | 不使用 | 额外抽象层；ESP-IDF 原生更利于实时与内存控制 |
| ArduinoJson | 不使用 | 协议 schema 固定，自研 JSON 子集更小、零堆分配 |
| cJSON（IDF 自带） | 不使用 | 默认用自研解析器；仅可作主机侧对照，不进固件 |
| NimBLE-Arduino | 不使用 | 直接使用 ESP-IDF 内置 NimBLE 协议栈 |
| LZ4 官方实现 | 固件内不使用 | LZ4 Block 格式简单、规范公开，自研解压器约百行级；官方实现（BSD-2）仅在主机侧作为**对照测试基准**（可选） |
| 步进电机库 | 不使用 | 自写加减速 |
| Unity | 使用（IDF 自带，L0） | 设备侧测试；主机侧可用自写的极简断言宏 |

---

## 三、关键前置条件与不可臆造项

开始编码前，先列出已知条件、未知条件和默认假设。不要因为信息不足就停止全部工作；对未知硬件采用接口隔离与模拟实现，并明确标注风险。

### 3.1 必须确认的硬件信息

- ESP32-S3 具体开发板型号、Flash 容量、PSRAM 容量与类型（Quad/Octal）及可用引脚。
- 喷头品牌、型号、接口定义、驱动板/驱动芯片、供电电压、点火时序、喷嘴数量与排列、最大频率及温控要求。
- 走纸电机型号、额定电流、驱动器型号、微步设置、限位/原点传感器。
- 是否确实存在切纸机构；若有，执行器类型、限位反馈及安全条件。
- 打印数据的像素格式、通道顺序、每行字节数、喷嘴映射、走纸方向、压缩算法及数据包最大尺寸。

### 3.2 安全原则

- 不得在未知喷头电气参数的情况下直接输出点火 GPIO 时序。
- 不得把 A4988 当作喷头驱动器；它只可能用于兼容的步进电机。
- 对未确认的切纸机构，`cut_paper` 默认返回 `UNSUPPORTED`，不得驱动未知引脚。
- 硬件 HAL 在默认配置下采用 `Mock` 模式；只有在硬件参数与驱动实现经过确认后才允许启用真实硬件。
- 所有缓冲区必须有明确容量上限、长度检查、超时和错误处理。
- 网络数据必须视为不可信输入，不能直接依据未经验证的长度分配任意内存。
- 断连、任务取消、重复包、乱序包、包损坏、解压失败及内存不足都必须有明确状态和恢复策略。
- 默认启用任务看门狗（TWDT）；打印期间任何任务卡死都必须能进入安全停机（停止点火、停止电机）。

---

## 四、框架选型与工程结构

### 4.1 选型结论：ESP-IDF（原生 CMake）

理由：

1. 直接使用 NimBLE、lwIP、FreeRTOS SMP、GPTimer、LCD_CAM、SPI-DMA 等官方组件，**零第三方封装层**。
2. 可精确控制任务绑核、内存属性（`MALLOC_CAP_SPIRAM` / `MALLOC_CAP_DMA` / `MALLOC_CAP_INTERNAL`）和中断优先级。
3. 通过 `idf.py` 构建，依赖关系透明，便于锁定版本与审计。
4. 将来对接喷头需要的精确时序，通常要用硬件定时器、DMA 与专用外设，ESP-IDF 层级更合适。

AI 若认为应选其他方案，必须在 Phase 0 以书面理由说明，并等待用户确认。

### 4.2 “可移植核心 + 平台适配”分层（关键设计）

为了便于主机侧单元测试与长期自主维护，代码分为两个互不反向依赖的层：

- **`core/`**：纯 C++，**不包含任何 ESP-IDF 头文件**。包含拆帧器、JSON 子集、命令参数校验、二进制帧状态机、LZ4、CRC、槽位池、打印状态机、流控、错误码。可以在 PC 上用 g++/clang 直接编译并跑单元测试与模糊测试。
- **`platform/`**：依赖 ESP-IDF，包含 TCP/BLE 服务、FreeRTOS 任务封装、PSRAM 分配器、HAL 实现、日志适配。通过 `core/` 中定义的抽象接口（如 `IClock`、`IAllocator`、`ITransportSink`）注入。

```text
firmware/
├── CMakeLists.txt
├── sdkconfig.defaults
├── partitions.csv
├── main/
│   ├── CMakeLists.txt
│   └── app_main.cpp
├── components/
│   ├── core/                      # 平台无关，可主机编译
│   │   ├── include/ src/
│   │   ├── errors.h
│   │   ├── config_limits.h        # 所有容量上限集中定义
│   │   ├── crc32.h/.cpp
│   │   ├── frame_splitter.h/.cpp  # '#' 拆帧
│   │   ├── json_lite.h/.cpp       # JSON 子集解析 + 写出器
│   │   ├── command_schema.h/.cpp  # 命令元数据与参数校验
│   │   ├── command_router.h/.cpp
│   │   ├── bin_frame_parser.h/.cpp
│   │   ├── lz4_block.h/.cpp
│   │   ├── slot_pool.h/.cpp
│   │   ├── flow_control.h/.cpp
│   │   ├── job_state_machine.h/.cpp
│   │   └── stats.h/.cpp
│   ├── platform_esp/              # 依赖 ESP-IDF
│   │   ├── wifi_manager.*
│   │   ├── control_tcp_server.*
│   │   ├── data_tcp_server.*
│   │   ├── ble_control_server.*   # 基于 IDF 内置 NimBLE
│   │   ├── psram_allocator.*
│   │   ├── task_layout.*
│   │   └── log_adapter.*
│   ├── hal/
│   │   ├── printhead_hal.h        # 纯接口
│   │   ├── stepper_hal.h
│   │   ├── cutter_hal.h
│   │   ├── mock_printhead.*       # 默认实现
│   │   ├── mock_stepper.*
│   │   └── mock_cutter.*
│   └── app/
│       ├── printer_app.*          # 组装各模块
│       └── device_info.*
├── host_tests/                    # 主机侧测试（g++ + 极简自写断言，无第三方）
│   ├── CMakeLists.txt
│   ├── test_frame_splitter.cpp
│   ├── test_json_lite.cpp
│   ├── test_bin_frame_parser.cpp
│   ├── test_lz4_block.cpp
│   ├── test_crc32.cpp
│   ├── test_slot_pool.cpp
│   ├── test_job_state_machine.cpp
│   └── fuzz_*.cpp                 # 自写随机变异 fuzz（固定种子，可复现）
├── tools/                         # 仅 Python 标准库
│   ├── make_packets.py            # 生成测试用二进制包（含简易 LZ4 编码器）
│   ├── send_job.py                # 控制+数据双通道模拟客户端
│   └── README.md
├── docs/
│   ├── protocols/                  # 第三方协议文档
│   ├── middleware.md               # Profile/HAL 接入
│   ├── memory_budget.md
│   └── hardware_questions.md
└── README.md
```

可按实际情况调整，但必须保持“`core` 不依赖 ESP-IDF”“`hal` 仅暴露接口”的边界，并解释任何删减。

### 4.3 编码规范

- 禁用异常与 RTTI；核心路径不使用 `std::string`、`std::vector`、`std::map` 等隐式堆分配容器；允许使用 `<cstdint>`、`<cstring>`、`<array>`、`<atomic>`。
- 数据路径零动态分配；初始化阶段一次性分配所有缓冲区，运行期不 malloc/free。
- 所有函数返回统一的 `Result`/错误码，不用裸 `bool` 掩盖原因。
- 打开编译告警 `-Wall -Wextra -Werror`，核心层开启 `-fno-exceptions -fno-rtti`。
- 每个模块头文件用注释写清：线程安全性、所有权、最大耗时、容量上限。

---

## 五、控制协议规范

### 5.1 通道划分

**A. 控制通道**

- BLE：使用 ESP-IDF 内置 NimBLE，自写 GATT 服务（一个写特征 + 一个通知特征）。
- Wi-Fi TCP 控制服务：接收和发送以 `#` 结尾的 JSON 报文。
- BLE 与 TCP 控制通道共享同一套 `frame_splitter → json_lite → command_router → 响应` 逻辑。
- BLE 单次写入受 MTU 限制（协商后通常远小于一条完整 JSON），因此必须依赖 `#` 终止符做**分片重组**；每个连接各自独立的重组缓冲区；重组有超时（默认 3 s 无新数据则丢弃半帧并返回错误）。
- BLE 通知同样需按 `MTU-3` 分片发送，接收端靠 `#` 重组。
- TCP 是字节流，不保留消息边界；解析器必须支持一条报文分多次到达，以及一次收到多条报文。

**B. 数据通道**

- 独立 Wi-Fi TCP Data Server，端口与控制端口不同，统一在 `config_limits.h` / 配置文件定义。
- 只承载打印任务的二进制分包，不混入控制 JSON。
- 默认仅允许 **一个** 活动数据连接；新连接在有活动任务时被拒绝。
- 数据连接与打印任务绑定，使用一次性令牌握手（见 6.1）。

### 5.2 通用 JSON 报文

每条控制消息以 `#` 结束，消息主体为 UTF-8 JSON：

```json
{
  "msg_id": "unique-message-id",
  "cmd": "get_device_info",
  "data": {}
}#
```

规则：

- `msg_id`：字符串，长度 1–40，响应原样带回；设备主动事件使用设备生成的 ID（如 `evt-<序号>`）。
- `cmd`：字符串，不能为空，必须在命令白名单内。
- `data`：对象。所有业务参数必须放在此字段中，不得散落到 JSON 顶层。
- 解析器**把第一个 `#` 视为帧结束符**。因此发送方必须保证 JSON 正文中不出现裸 `#`，字符串里的 `#` 必须写成 `\u0023`。设备端写出 JSON 时同样执行此转义。**这是协议的强制约束，必须在文档与测试中体现。**
- 定义 `MAX_CONTROL_FRAME_SIZE`（建议默认 2048 字节）。超限则丢弃至下一个 `#` 并返回 `FRAME_TOO_LARGE`；缓冲区不得增长。
- JSON 解析失败、字段缺失、类型错误、未知命令均返回标准错误响应；无法提取 `msg_id` 时，响应中 `msg_id` 为 `null`。
- 空帧（只有 `#`）直接忽略，不回包。

统一响应格式（成功与失败结构一致）：

```json
{
  "msg_id": "unique-message-id",
  "cmd": "get_device_info",
  "data": {
    "success": true,
    "result": {},
    "error": null
  }
}#
```

错误示例：

```json
{
  "msg_id": "abc",
  "cmd": "start_print",
  "data": {
    "success": false,
    "result": null,
    "error": { "code": "INVALID_ARGUMENT", "message": "total_packets must be greater than zero" }
  }
}#
```

### 5.3 自研 JSON 子集规范（`json_lite`）

目标：用最小实现安全处理本协议，而不是实现完整 JSON。

**解析器要求**

- 就地分词：对输入缓冲区只读或原地处理，输出 token 数组（固定容量，建议 ≤ 64 个 token），不分配堆内存。
- 支持类型：object、array、string、integer、boolean、null。**浮点数不在 V1 支持范围**，遇到小数或指数返回 `INVALID_ARGUMENT`（如后续确需浮点再扩展）。
- 限制：嵌套深度 ≤ 4；单个字符串 ≤ 128 字节（解码后）；整数范围 int64，解析时检查溢出；同一对象内重复键视为错误。
- 字符串：支持 `\" \\ \/ \b \f \n \r \t \uXXXX`；UTF-8 输入需做合法性校验；遇到非法转义、控制字符、孤立代理项一律拒绝。
- 未知字段处理策略必须固定：**对已知命令，未知字段拒绝并报 `INVALID_ARGUMENT`**（严格模式，利于早发现协议偏差）。
- 查找字段使用线性查找 token（规模小），提供 `getString/getInt/getBool/getObject` 等类型安全接口，类型不符返回错误而非默认值。

**写出器要求**

- 流式写入到调用方提供的定长缓冲区，每次写入检查剩余容量，溢出时整条报文标记失败并返回 `INTERNAL_ERROR`，**不得输出被截断的 JSON**。
- 自动转义（含 `#`）；整数输出自写，不依赖 `printf` 浮点。
- 大响应（如 `get_device_info`）可分块写入发送队列，避免超大栈缓冲。

### 5.4 `get_device_info`

响应的 `data.result` 至少包含：

- `device`：设备 ID、型号、固件版本、协议版本、支持的连接方式、能力状态。
- `printhead`：品牌、型号、喷嘴数量、分辨率、最大打印宽度、接口类型等。未知值必须使用 `null`、`"unknown"` 或明确的 `is_configured:false`，禁止编造。
- `memory`：PSRAM 是否可用、总量与当前可用量（能可靠读取时才上报）。
- `capabilities`：是否支持彩色、压缩算法、数据分包、走纸、切纸、模拟打印、CRC 校验等。
- `limits`：`max_control_frame`、`max_packet_compressed`、`max_packet_uncompressed`、`max_job_bytes`、`queue_depth`、控制/数据端口。
- `commands`：当前固件实际支持的命令清单与参数元数据。

命令元数据**由 `command_schema` 单一数据源生成**，同时用于参数校验与 `get_device_info` 输出，从结构上保证“上报支持的命令 = 实际路由的命令”。

```json
{
  "label": "开始打印",
  "description": "创建打印任务并初始化有界缓存",
  "params": [
    {
      "key": "total_bytes_uncompressed",
      "label": "解压后总字节数",
      "type": "integer",
      "required": true,
      "default_value": null,
      "options": [],
      "unit": "bytes",
      "min": 1,
      "max": 67108864,
      "description": "本任务全部解压后数据的预期总长度"
    }
  ]
}
```

`commands` 至少覆盖：`get_device_info`、`start_print`、`cancel_print`、`get_print_status`、`feed_paper`、`cut_paper`。只有 HAL 确实支持时，才将 `feed_paper`、`cut_paper` 报告为可执行；否则返回 `UNSUPPORTED`，且元数据中标记 `"supported": false`。

### 5.5 `start_print`

请求示例：

```json
{
  "msg_id": "job-create-001",
  "cmd": "start_print",
  "data": {
    "job_id": "job-001",
    "total_bytes_uncompressed": 120000,
    "total_packets": 30,
    "compression": "lz4_block",
    "integrity": "crc32",
    "bytes_per_row": 1200,
    "total_rows": 100,
    "cutter_after_finish": false
  }
}#
```

校验要求：

- 所有整数先验证类型、范围和乘法溢出（使用无溢出检查函数，如 `mul_overflow`），再用于分配或索引。
- `total_packets > 0`，`total_bytes_uncompressed > 0`，`bytes_per_row > 0`，`total_rows > 0`，`job_id` 长度 1–40 且只允许 `[A-Za-z0-9_-]`。
- `bytes_per_row * total_rows == total_bytes_uncompressed`。若格式允许行尾填充，必须在协议中明确定义，否则严格相等。
- `total_bytes_uncompressed <= total_packets * MAX_PACKET_UNCOMPRESSED_SIZE`，且 `total_packets <= total_bytes_uncompressed`（每包至少 1 字节）。
- `compression` ∈ {`none`, `lz4_block`}；`integrity` ∈ {`none`, `crc32`}（缺省为 `none`）；未实现的值必须拒绝。
- `total_bytes_uncompressed <= MAX_JOB_BYTES`。
- 在任务创建成功之前不得接受数据包；同一时间默认只允许一个活动任务，否则返回 `JOB_ALREADY_RUNNING`。
- 成功响应包含：`job_id`、状态、`data_port`、`data_token`（32 位随机数，由硬件 RNG 产生，一次性有效）、接受的容量限制、协议参数摘要、`data_connect_timeout_ms`。
- 若在 `data_connect_timeout_ms` 内没有合法数据连接，任务以 `JOB_TIMEOUT` 终止。

---

## 六、二进制分包协议（字节序：Little Endian）

### 6.1 数据连接握手

数据连接建立后，客户端先发送 **12 字节** Hello（仅一次，不属于分包帧）：

```text
+---------------+----------------+----------------------+
| Magic 4B      | Version 2B LE  | Token 4B LE | Rsv 2B |
| "IJPD"        | 0x0001         | data_token  | 0x0000 |
+---------------+----------------+----------------------+
```

设备校验：魔数、版本、`data_token` 与当前任务一致且未被使用、保留字段为 0。失败立即断开并记录 `DATA_CONNECTION_LOST` 或 `INVALID_ARGUMENT`。此机制使数据连接与任务严格绑定，并避免陌生设备注入数据。

> 若用户认为握手增加了 APP 侧工作量，可在 Phase 0 提出裁剪方案，但必须保留“令牌或等效绑定机制”。

### 6.2 分包帧格式

```text
+-------------------+---------------------+-------------------+---------------------+---------------+
| Packet Index (2B) | Uncompressed Len(2B)| Compressed Len(2B)| Payload             | CRC32 (4B)    |
|                   |                     |                   | Compressed Len 字节 | 仅 integrity= |
|                   |                     |                   |                     | crc32 时存在  |
+-------------------+---------------------+-------------------+---------------------+---------------+
```

强制规则：

- 所有整数统一采用 **Little Endian**；在 `core` 中提供明确的 `read_u16_le/read_u32_le`，禁止通过指针强转读取（避免对齐与字节序问题）。
- `Packet Index` 从 `0` 开始，范围 `0 ... total_packets-1`。因 16 位限制，**单任务最多 65535 个包**（`total_packets` 校验时拒绝更大值）。
- `Uncompressed Len` 与 `Compressed Len` 均为无符号 16 位，单包最大 65535 字节；`Uncompressed Len` 必须 ≥ 1，且 ≤ `MAX_PACKET_UNCOMPRESSED_SIZE`。
- 除最后一包外，各包 `Uncompressed Len` 必须等于 `start_print` 约定的标称包长（由 APP 与设备固定，建议在响应中回显 `nominal_packet_bytes`）；最后一包可短。累计 `Uncompressed Len` 必须恰好等于 `total_bytes_uncompressed`。
- **CRC32 范围**：覆盖 6 字节头 + 载荷（不含 CRC 本身），多项式使用标准 CRC-32/ISO-HDLC（与 zlib 一致），附测试向量 `"123456789" → 0xCBF43926`。校验失败返回 `PACKET_CRC_MISMATCH`（须列入错误码表）。
- 每个 TCP `read()` 可能只得到半个包，也可能包含多个包；必须使用状态机（`HEADER → PAYLOAD → CRC → DISPATCH`）按字节累积，不假定一次读取等于一个完整帧。
- **严格顺序模式（V1 固定策略）**：只接受预期序号。重复包、乱序包、缺包一律判定 `PACKET_OUT_OF_ORDER`，终止任务并通过控制通道上报错误，不做静默丢弃。
- 如后续需要重传，设计 ACK/NACK 与超时作为后续协议版本的扩展，不得在 V1 帧中静默增加字段。

### 6.3 压缩数据规则

- `compression = none`：必须 `Compressed Len == Uncompressed Len`，载荷直接作为像素数据。
- `compression = lz4_block`：
  - 每包独立压缩，**独立 LZ4 Block**，不依赖前包字典，不使用 LZ4 Frame 格式。
  - **“存储兜底”规则（解决 16 位长度的歧义）**：编码端若 LZ4 输出长度 ≥ 原始长度，则改发原始数据（此时 `Compressed Len == Uncompressed Len`）。**因此，在 `lz4_block` 模式下，`Compressed Len == Uncompressed Len` 一律表示该包是原始数据，不进入解压；`Compressed Len < Uncompressed Len` 才是 LZ4 数据；`Compressed Len > Uncompressed Len` 一律非法。** 编码端须保证永不输出“长度相等的真 LZ4 流”。
  - 因此 APP 端在 `lz4_block` 模式下的标称包长建议不超过 65535，且无需担心 LZ4 膨胀越过 16 位。

### 6.4 自研 LZ4 Block 解压器规范

函数原型（示例）：

```cpp
// 成功返回解压后字节数；失败返回负的错误码。绝不写出 dst_cap 之外，绝不读取 src_len 之外。
int lz4_block_decompress(const uint8_t* src, size_t src_len,
                         uint8_t* dst, size_t dst_cap,
                         size_t expected_len);
```

必须满足：

1. 严格实现 LZ4 Block 序列格式：token 高 4 位为 literal 长度、低 4 位为 matchlen−4；值为 15 时用后续字节累加扩展（每个字节为 255 则继续）。
2. 每次读取（token、长度扩展字节、literal、offset）前检查剩余输入；每次写入前检查剩余输出；长度累加检查整数溢出。
3. `offset == 0` 或 `offset >` 已输出字节数 → 拒绝（`DECOMPRESSION_FAILED`）。
4. 匹配复制必须正确处理**重叠**（offset < length 时按字节顺序复制，不可用 `memcpy`/`memmove`）。
5. 最后一个序列只含 literal、无 offset；遇到输入耗尽即结束。输入耗尽的位置必须落在合法的序列边界上，否则判定损坏。
6. 最终输出长度必须**严格等于** `expected_len`，否则 `DECOMPRESSED_LENGTH_MISMATCH`。
7. 解压输出直接写入槽位缓冲区，避免二次拷贝。
8. 不使用递归；最坏耗时与输出长度线性相关，需在注释中给出估算。
9. 允许在后续优化中使用 32 位宽拷贝（须保证对齐与边界安全，并保留慢速参考路径用于对照测试）。

测试要求：

- 人工构造的向量：纯 literal、短匹配、长匹配（长度扩展字节）、offset=1 的重叠（如游程）、长 literal 扩展。
- 对照测试：`tools/make_packets.py` 内自带简易 LZ4 编码器（贪心哈希匹配即可），随机数据与图像样式数据（大面积重复）回环验证。
- 可选：主机侧使用官方 LZ4 命令行/库生成数据来交叉验证（该依赖仅限开发机，不进入固件与仓库构建）。
- 恶意输入：截断流、offset 越界、长度扩展越界、输出溢出、全随机字节，**必须无崩溃、无越界**（主机侧用 `-fsanitize=address,undefined` 运行）。

---

## 七、流式打印流水线

### 7.1 数据流

```text
Wi-Fi TCP Data Server
        │  (字节流，lwIP 接收，按需读取以形成背压)
        ▼
二进制帧状态机（有界缓冲）
        │
        ▼
序号 / 长度 / 任务状态 / CRC32 校验
        │
        ▼
解压器（LZ4 Block 或 none）
        │
        ▼
PSRAM 槽位池（固定数量、固定大小槽位）
        │
        ▼
打印调度器（按行/带/喷嘴组向 HAL 提交）
        │
        ├──► 喷头 HAL（默认 Mock）
        └──► 走纸电机 HAL（默认 Mock）
        │
        ▼
完成计数 / 状态机 / 事件回报
```

### 7.2 缓冲和内存规则

- **槽位池（SlotPool）**：启动或任务创建时一次性从 PSRAM 分配 `N` 个固定大小槽位（默认建议 `N = 6`，槽位大小 = `MAX_PACKET_UNCOMPRESSED_SIZE`，即约 64 KB，合计约 384 KB；以 8 MB PSRAM 为默认假设，最终值在 Phase 0 的内存预算中确定）。
- 槽位状态机：`FREE → FILLING → READY → PROCESSING → FREE`，转换由槽位池集中提供原子接口，禁止生产者覆盖消费者正在处理的槽位。
- 队列元素只保存槽位索引、包序号、有效长度、**任务代次（generation）**，不拷贝图像数据。任务取消后，代次不匹配的残留元素必须被丢弃。
- 压缩包接收缓冲：独立的固定缓冲区（建议 2 × 64 KB，PSRAM），不与槽位混用。
- **DMA 与内存属性**：凡将来需要被 DMA 读取的缓冲区，必须明确是否位于内部 SRAM 或支持 PSRAM DMA 的路径，并注意 Cache 一致性与对齐要求；Phase 7 之前在 `memory_budget.md` 中标注“待硬件确认”。
- PSRAM 不可用时：按配置拒绝打印任务（默认），或退回严格受限的小缓存模式（需显式开启，并在 `get_device_info` 中上报）。
- 所有分配失败都要检查；任务结束路径统一释放；使用 `heap_caps_get_free_size` / 最小水位记录监控泄漏。
- 必须配置：`MAX_PACKET_COMPRESSED_SIZE`、`MAX_PACKET_UNCOMPRESSED_SIZE`、`MAX_JOB_BYTES`、`SLOT_COUNT`、`HIGH_WATERMARK`、`LOW_WATERMARK`，全部集中在 `config_limits.h`，并用 `static_assert` 检查自洽性。
- 统计项：队列水位与历史最大水位、包计数、解压失败计数、CRC 失败计数、吞吐量（B/s）。

### 7.3 FreeRTOS 任务划分（SMP 双核）

| 任务 | 职责 | 建议核心 | 建议优先级（相对） | 备注 |
|---|---|---|---|---|
| Wi-Fi/lwIP | 系统自带 | Core 0 | 系统设定 | 不干预 |
| 数据接收 `rx_task` | 读 TCP、帧状态机、校验 | Core 0 | 中 | 可阻塞等待槽位 |
| 解压 `decode_task` | LZ4/none 解压到槽位 | Core 1 | 中高 | 与接收可合并，需说明 |
| 打印调度 `print_task` | 向 HAL 提交数据并协调走纸 | Core 1 | 最高（应用层） | 不做 JSON、不做网络发送 |
| 控制 `ctrl_task` | 命令解析与响应 | Core 0 | 低 | |
| 状态上报 `report_task` | 事件序列化与发送 | Core 0 | 低 | 通过事件队列接收，避免在实时任务序列化 |

- 以上优先级和栈大小仅为起点，必须用 `uxTaskGetStackHighWaterMark` 实测后在文档中固定，并留出安全余量。
- 避免忙等；使用 Queue、Semaphore、TaskNotification、EventGroup 同步。共享状态必须有明确的锁或“单任务所有权”规则，在文档中列出每个共享变量的所有者。
- 不得在 `print_task` 或 ISR 中执行 JSON 序列化、阻塞网络发送、日志刷屏或长时间内存复制。
- 打印期间任务加入 TWDT；`print_task` 异常时触发安全停机回调（HAL `stop()`）。

### 7.4 流控

- 当 READY + PROCESSING 槽位数达到高水位时，`rx_task` 停止从 socket 读取，借助 TCP 窗口形成背压；降到低水位后恢复。
- TCP 背压不等价于应用层 ACK；V1 同时周期性（如每 N 包或每 200 ms）通过控制通道上报 `print_progress` 事件，供 APP 参考并限制发送端窗口。
- 所有等待必须带超时并能被取消事件唤醒：`xQueueReceive` / `xSemaphoreTake` 使用有限超时，循环中检查取消标志。
- 队列满时不得覆盖未消费数据，也不得无限阻塞，必须保证 `cancel_print` 随时可响应。

---

## 八、打印任务状态机与闭环

推荐状态：

```text
IDLE
  └── start_print 合法 ──► PREPARING
                              ├── 初始化失败 ──► ERROR
                              └── 初始化成功 ──► WAITING_DATA_CONN
                                                    ├── 超时 ──► ERROR(JOB_TIMEOUT)
                                                    └── 握手成功 ──► RECEIVING
                                                                       └── 所有包接收 ──► DRAINING
                                                                              └── 所有数据打印完成 ──► FINALIZING
                                                                                    ├── 切纸不支持/失败 ──► DONE_WITH_WARNING
                                                                                    └── 收尾成功 ──► COMPLETED
```

另需支持 `CANCELLING`、`CANCELLED`、`ERROR`。**所有状态转换由 `job_state_machine` 单点实现**，非法转换返回错误并记录日志；在主机侧用表驱动测试覆盖全部合法/非法转换。终态（`COMPLETED/DONE_WITH_WARNING/CANCELLED/ERROR`）在下次 `start_print` 前保持，可被 `get_print_status` 查询。

### 8.1 完成条件

不能只以“收齐所有 TCP 包”判定打印完成。至少区分并全部满足：

- `packets_received == total_packets`
- `packets_decoded == total_packets`
- `bytes_decoded == total_bytes_uncompressed`
- `print_scheduler_completed == true`
- 所有槽位已回到 `FREE`，且无待执行打印数据

### 8.2 完成响应

```json
{
  "msg_id": "evt-0007",
  "cmd": "print_finished",
  "data": {
    "success": true,
    "result": {
      "job_id": "job-001",
      "status": "completed",
      "packets_received": 30,
      "packets_decoded": 30,
      "bytes_printed": 120000,
      "duration_ms": 12345,
      "warning": null
    },
    "error": null
  }
}#
```

- 事件默认**广播到所有已连接控制端**（BLE 与 TCP），并支持通过 `get_print_status` 补查。发送失败仅记录，不得因此重复执行物理打印。
- 事件发送使用独立事件队列和序号，避免阻塞打印路径。

### 8.3 取消与异常

- `cancel_print` 必须使接收、解压、打印调度都能安全退出：置取消标志 → 递增任务代次 → 唤醒各等待点 → HAL `stop()` → 回收槽位 → 关闭数据连接 → 进入 `CANCELLED`。
- 数据 TCP 断开：V1 默认**中止任务**并上报 `DATA_CONNECTION_LOST`。
- 解压错误、序号错误、CRC 错误、超长包、任务超时、PSRAM 不足、HAL 故障均返回标准错误码并进入 `ERROR`。
- 切纸失败不覆盖真实打印结果，进入 `DONE_WITH_WARNING`。
- 任务总超时与“包间空闲超时”分别可配置。

---

## 九、硬件抽象层（HAL）边界

### 9.1 喷头 HAL

接口示例：

```cpp
class IPrintheadHal {
public:
    virtual ~IPrintheadHal() = default;
    virtual Result begin(const PrintheadConfig&) = 0;
    virtual Result submitRaster(const uint8_t* data, size_t len, const RasterMeta&) = 0;
    virtual Result waitUntilIdle(uint32_t timeout_ms) = 0;
    virtual Result stop() = 0;            // 必须可在任意状态安全调用，幂等
    virtual HalStatus getStatus() const = 0;
};
```

- 真实喷头型号未确认前只提供 `MockPrintheadHal`，可配置“每行耗时、故障注入”，用于验证上层流程。
- 真实驱动依据厂商资料实现：电气接口、驱动芯片、点火时序、喷嘴映射、温度与维护机制。
- 实现手段限定为 ESP-IDF 官方外设（GPTimer、RMT、LCD_CAM、SPI + GDMA、MCPWM、PCNT、ETM 等）；**不使用未经审批的第三方驱动库**。

### 9.2 走纸电机 HAL

- 独立接口，支持“走纸距离/步数、速度、梯形加减速、停止、状态反馈”。
- 步进脉冲由 GPTimer 或 MCPWM 生成，自研梯形加减速表（预计算，避免 ISR 中浮点与除法）。
- A4988 仅作为兼容步进电机驱动器的候选方案；需确认电机电流、供电、限位和加减速要求。
- 真实实现必须有原点/限位或其他可靠的位置校准策略，不能仅靠延时假定走纸位置正确。

### 9.3 切纸 HAL

- 默认 `UNSUPPORTED`；只有检测到实际硬件配置且安全条件满足时才执行。
- `cutter_after_finish=true` 但设备不支持切纸时，打印任务正常完成并附带警告，不得误驱动 GPIO。

---

## 十、错误码与可观测性

至少定义以下错误码（集中于 `errors.h`，并提供“码 → 字符串”的唯一映射）：

`INVALID_JSON`、`INVALID_ARGUMENT`、`UNKNOWN_COMMAND`、`FRAME_TOO_LARGE`、`FRAME_TIMEOUT`、`PACKET_OUT_OF_ORDER`、`PACKET_LENGTH_INVALID`、`PACKET_CRC_MISMATCH`、`DECOMPRESSION_FAILED`、`DECOMPRESSED_LENGTH_MISMATCH`、`QUEUE_TIMEOUT`、`PSRAM_UNAVAILABLE`、`OUT_OF_MEMORY`、`JOB_ALREADY_RUNNING`、`NO_ACTIVE_JOB`、`DATA_CONNECTION_LOST`、`DATA_HANDSHAKE_FAILED`、`HAL_NOT_CONFIGURED`、`HAL_FAULT`、`UNSUPPORTED`、`JOB_CANCELLED`、`JOB_TIMEOUT`、`INTERNAL_ERROR`

日志要求：

- 日志级别可配置，数据热路径不逐字节/逐包打印；高频事件使用计数器 + 周期汇总。
- 记录任务 ID、包序号、错误码、状态转换、缓冲区水位和耗时。
- 不得记录敏感凭据（Wi-Fi 密码、令牌等）。
- 可选：提供 `get_diagnostics` 命令（需同步更新能力元数据），输出统计与堆水位。

---

## 十一、构建配置

### 11.1 ESP-IDF 构建要求

AI 必须提供可用的：

- 顶层与组件的 `CMakeLists.txt`；
- `sdkconfig.defaults`：目标 `esp32s3`、启用 PSRAM（类型与频率按实际板卡填写，未知则标注占位）、双核 FreeRTOS、TWDT、NimBLE 仅启用外设角色（Peripheral）、合理的 lwIP TCP 窗口/缓冲（给出取值依据并标注需实测）、关闭不用组件以缩减体积；
- `partitions.csv`（按实际 Flash 容量）；
- README 中列明：锁定的 ESP-IDF 版本、安装步骤、`idf.py set-target esp32s3`、`idf.py build`、`idf.py -p <PORT> flash monitor`、常见问题。

要求：运行时必须检查 PSRAM 初始化结果与剩余容量，不能只依赖 `sdkconfig`。

### 11.2 主机测试构建

- `host_tests/CMakeLists.txt` 仅依赖系统编译器；`core/` 在 PC 上以 `-Wall -Wextra -Werror -fsanitize=address,undefined` 编译并运行。
- 测试框架：自写极简断言头（`CHECK_EQ`、`CHECK_TRUE` 并输出行号），不引入 gtest/Catch。
- 固件侧仅少量集成测试使用 IDF 自带 Unity。

### 11.3 依赖与版本

- 除 ESP-IDF 外，`third_party/` 默认为空；如有审批通过的引入，必须附 LICENSE、来源 URL、版本与提交哈希，并 vendoring 到仓库。
- 禁止使用 `latest`、浮动版本或构建时在线拉取源码。
- Arduino IDE 与 PlatformIO 不作为交付路径；如用户明确要求，另行评估。

---

## 十二、测试与验收

必须提供可复现的测试步骤和预期结果，不能只说“测试通过”。

### 12.1 控制协议测试（主机侧）

- 一条 JSON 分多次接收，最终只解析一次。
- 一次接收多条以 `#` 结尾的 JSON，逐条解析。
- 缺少 `#`（超时）、超长帧、非法 JSON、缺少 `data`、未知命令、重复键、未知字段、深度超限、整数溢出、非法 UTF-8、含 `\u0023` 的字符串。
- BLE 分片重组：按 20/100/244 字节切片随机投喂；分片超时、超长。
- 写出器：缓冲区刚好满/溢出时不输出截断 JSON。

### 12.2 二进制分包测试（主机侧）

- 正常完整包、逐字节投喂（极端半包）、多包粘连、随机切分。
- 错误序号、重复序号、缺包、长度字段超限、`Compressed Len > Uncompressed Len`、总字节不符。
- CRC32 标准向量；单比特翻转检测。
- `none` 模式长度不匹配；`lz4_block` 存储兜底（长度相等）分支。
- LZ4：向量测试、回环测试、恶意输入测试（ASan/UBSan 下无告警）。
- **Fuzz**：固定随机种子的变异测试（对合法帧随机改写字节/截断/拼接），运行不少于 N 万次迭代（默认 10 万）无崩溃、无越界、无死循环。

### 12.3 流水线与状态机测试

- 槽位池：并发生产者/消费者压力下状态转换合法，不出现覆盖。
- 高低水位暂停/恢复；队列满时取消命令仍可响应。
- 模拟喷头延迟、HAL 故障注入、取消任务（在每个状态下取消）。
- 所有包已接收但打印队列尚未清空时，不得提前回报完成。
- 连续执行 ≥ 100 次 Mock 任务后，堆空闲量与最小水位无持续下降。
- 状态机表驱动测试：合法转换全覆盖、非法转换全拒绝。

### 12.4 集成测试（设备侧 + `tools/send_job.py`）

- `send_job.py`（仅标准库 `socket/struct/json/zlib.crc32`）模拟 APP：控制连接发 `start_print`，数据连接握手并发送分包，打印进度与完成事件。
- 覆盖：正常任务、中途断开、中途取消、错误序号、CRC 损坏、数据连接超时。

### 12.5 验收条件

- 工程可在锁定版本的 ESP-IDF 环境编译，零警告（含 `-Werror`）。
- 主机侧全部单元测试与 fuzz 通过，且 ASan/UBSan 无告警。
- Mock HAL 模式下完整走通“创建任务 → 握手 → 发送数据 → 解析 → 解压/校验 → 入槽 → 模拟打印 → 完成回报”闭环。
- 错误路径均能返回标准错误，不死锁、不越界、不无限分配。
- `git` 仓库中 `third_party/` 为空，或每个第三方库均有审批记录。
- 文档明确区分“已真实实现”“Mock 模拟”“依赖硬件资料后才能实现”的功能。
- 未接入真实喷头驱动前，不得宣称已完成真实喷墨打印。

---

## 十三、分阶段开发流程（强制）

每一阶段都必须输出完整文件，而不是只输出差异片段。

### Phase 0：协议与工程决策

输出：
- 已知条件、未知条件、默认假设清单。
- 对 ESP-IDF 选型的确认（或书面的替代方案理由）。
- 控制协议与二进制帧格式定稿（含 `#` 转义、数据握手、CRC32、存储兜底规则）。
- 《依赖清单》：确认 `third_party/` 为空，或列出申请表。
- 缓冲区预算（含 PSRAM/内部 SRAM 分配表）、最大包长、最大任务大小、任务栈与优先级初值及依据。
- 风险清单与待确认硬件问题（`docs/hardware_questions.md`）。

### Phase 1：`core` 基础设施 + 主机测试

实现：`errors.h`、`config_limits.h`、`crc32`、`frame_splitter`、`json_lite`（解析 + 写出）、主机测试工程。
验收：主机编译、单元测试、ASan/UBSan 全部通过。

### Phase 2：命令模型与路由

实现：`command_schema`、`command_router`、`device_info`；六个命令的路由与参数校验；元数据与路由同源。
验收：主机侧可模拟收发完整命令；不支持的硬件动作明确返回 `UNSUPPORTED`。

### Phase 3：二进制协议与 LZ4

实现：`bin_frame_parser`、`lz4_block`、`tools/make_packets.py`（含测试用 LZ4 编码器）、fuzz。
验收：正常/异常/恶意输入全部符合预期；明确列出 LZ4 解压器测试覆盖范围。

### Phase 4：槽位池、流控、状态机

实现：`slot_pool`、`flow_control`、`job_state_machine`、`stats`；主机侧多线程压力测试。
验收：不覆盖数据、不死锁、取消可在任意状态完成。

### Phase 5：ESP-IDF 平台层

实现：Wi-Fi 管理、控制 TCP、数据 TCP、BLE（IDF 内置 NimBLE）、PSRAM 分配器、任务布局、日志适配；`app_main`。
验收：设备上控制/数据通道独立运行，半包/粘包/BLE 分片均正确；`idf.py build` 零警告。

### Phase 6：Mock HAL 与端到端闭环

实现：Mock 喷头/走纸/切纸、打印调度任务、完成/警告/取消/错误事件、`tools/send_job.py`。
验收：无真实打印头下完整走通任务闭环；连续 100 次任务无泄漏；实测吞吐量与水位记录入文档。

### Phase 7：真实硬件适配

在用户提供喷头、驱动板、电机与传感器的具体型号及官方资料后，再实现真实 HAL。必须先输出：
- 硬件接口与电气风险评估。
- 引脚分配表（含与 PSRAM/Flash/USB/JTAG 保留引脚的冲突检查）。
- 驱动时序设计（所选 ESP-IDF 外设、DMA 路径、定时精度分析）。
- 保护与异常恢复方案（欠压、过温、卡纸、通信中断时的安全停机）。
- 台架测试步骤（先无墨水、空载、示波器/逻辑分析仪验证，再接入喷头）。

未经确认不得生成可能损坏喷头或硬件的点火代码。

### Phase 8：最终集成与交付

- 完整工程目录和所有源文件。
- `README.md`（依赖、构建、烧录、测试、故障排查）。
- `docs/protocols/`（当前协议文档）与示例 JSON/二进制包生成脚本。
- 测试清单、覆盖说明、已知限制。
- 明确标注已完成、模拟实现、待硬件验证的功能。
- 《依赖审计报告》：列出所有外部代码及其许可证。

---

## 十四、AI 编程助手执行规则

1. **先检查现状再修改：** 如果工作区已有工程，先检查文件、IDF 版本和现有实现，优先增量修改，不得未经确认就推倒重建。
2. **完整文件优先：** 每次修改输出完整文件内容、文件路径和覆盖说明，不只提供零散 diff。
3. **自研优先，依赖审批：** 任何新增外部代码（含“只是一个小头文件”）都必须先提交《依赖申请表》并等待确认；不得以“省事”为由引入库。
4. **禁止伪实现：** 不得用空函数、固定成功返回值或“桩函数”冒充真实功能。Mock 实现必须明确命名并标注用途。
5. **逐阶段验收：** 每阶段给出构建命令、测试步骤、预期输出和实际结果；无法实际运行时，必须说明“未在真实环境验证”，不得宣称通过。
6. **不擅自改变协议：** 如需修改字段、字节序、帧格式或状态机，先解释兼容性影响并提出变更方案。
7. **不臆造硬件参数：** 未知的喷头、电机、切纸器、电气参数必须保留为待配置项。
8. **资源安全优先：** 所有网络输入、长度、索引、内存分配和并发共享状态都必须校验；数据路径零动态分配。
9. **不要一次输出无法核验的巨型代码堆：** 按 Phase 顺序输出，每阶段结束后等待用户或测试结果确认，再继续下一阶段。
10. **核心可移植：** `core/` 严禁包含 ESP-IDF 头文件；平台差异一律通过接口注入。
11. **输出语言：** 中文说明和注释为主；代码标识符使用清晰一致的英文命名。
12. **最终交付必须可追踪：** 给出文件清单、版本信息、已知限制、硬件依赖项与依赖审计结果。

---

## 十五、最终启动指令

请从 **Phase 0：协议与工程决策** 开始，不要立即一次性生成全部固件。

先输出：
1. 已知条件、未知条件和默认假设。
2. 对 ESP-IDF 选型与“自研优先”策略的确认，以及自研模块清单与工作量/风险评估。
3. V1 控制 JSON 与二进制分包协议的最终规范（含 `#` 转义、数据握手、CRC32、存储兜底）。
4. 初始内存与缓冲区预算（PSRAM / 内部 SRAM 分配表）与任务布局建议。
5. 依赖清单（默认无第三方）；如认为有必要引入某库，按《依赖申请表》说明。
6. 风险清单和需要用户后续确认的硬件信息。
7. Phase 0 的验收清单。

完成 Phase 0 后停止，等待确认再进入 Phase 1。
