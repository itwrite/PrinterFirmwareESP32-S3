# RICOH MH5440 Mock HAL 方案

> 状态：`simulation_only`。当前没有真实喷头、驱动板、电源和厂商控制资料。
> 本方案只模拟应用层可验证的“光栅行 -> 逻辑喷头 -> 纸路/状态”契约，
> 不模拟未经资料确认的点火电压、波形、连接器时序或真实喷嘴寻址。

## 1. 目标和边界

### 目标

- 在没有真实喷头的情况下，验证 IJPD 光栅流水线、行宽校验、CMYK 通道顺序、吞吐、取消、故障终态和数据指纹。
- 让 Mock 和未来真实驱动共享 `IPrintheadHal`、`PrintPipeline`、`PrintService`、Profile 和协议层。
- 将未来真实驱动限制在“喷头策略 + 控制器/总线”适配层，避免修改 TCP、BLE、MQTT、IPP 或上层 Job 状态机。
- 使用确定性输出和故障注入，保证主机测试、ESP32 测试和后续硬件回归可以比较。

### 不做的事情

- 不根据公开产品页猜测 MH5440 的驱动波形、点火脉宽、电压、电流、连接器 pinout 或串行协议。
- 不把“缺失喷嘴率”仿真结果解释为真实喷嘴可靠性数据。
- 不把 600 dpi、A4、carriage speed 或纸张走纸速度写成 MH5440 厂商规格。
- 不让 Mock 直接调用 GPIO、SPI、LCD_CAM、RMT 或未知电源控制。
- 不在 `PrintService` 中加入 Ricoh 专用协议和硬件时序。

## 2. 公开资料和事实等级

官方产品页：

<https://www.ricoh.com/products/industrial-inkjet-printheads/ricoh-mh5420-mh5440>

官方页面可确认的 MH5440 信息：

| 项目 | 官方页面可见信息 | 在本项目中的用法 |
|---|---|---|
| 类型 | 压电式、金属隔膜 piston pusher | 仅用于 profile 说明 |
| 喷嘴 | `1,280`，`4 x 320`，交错排列 | 用于逻辑几何模型，不等同于输入行格式 |
| 喷头宽度 | `54.1 mm` | 作为单个 head 的物理宽度上限说明 |
| 四色喷嘴间距 | `1/150 inch`，约 `0.1693 mm` | 作为 channel pitch 说明 |
| 行间距 | `0.55 mm` | 只作为未来物理调度参数，当前不驱动 |
| 原生分辨率 | 官方页面描述为 `600 dpi` | 不直接推导 `bytes_per_row` |
| 四色配置 | 四色时每色一列 150 dpi 的排列说明 | 用于 Mock 的 channel geometry 记录 |
| 灰阶 | 最多 4 levels | Mock 可统计 8-bit 输入，不宣称真实 4-level 转换 |
| 频率 | binary `30 kHz`、gray-scale `20 kHz` | 只用于 capability/reference，不作为 ESP32 时序 |
| 温度 | 最高约 `60 C`，内置 heater/thermistor | 只用于故障模型字段，不读取真实温度 |
| 液滴 | binary `7 pl`，gray-scale `7-35 pl`，依墨水而定 | 不用于像素到液滴的真实换算 |

以下内容仍然未知，必须等驱动板/厂商资料确认：

- 控制器型号和主控接口。
- 逻辑电平、喷头电源、加热器电源和保护时序。
- 点火波形、充电/选通顺序、reset 时序和最大连续工作窗口。
- 连接器 pinout、数据位序、时钟边沿、latch/strobe 语义。
- 交错喷嘴的真实物理编号、通道顺序和行反转规则。
- 多 pass、走纸补偿、首行/末行偏移、维护喷射和清洗动作。

## 3. 当前仓库状态

当前代码已经有一个通用 HAL 边界：

- `components/printer_hal/include/printer_hal/printhead_hal.h`
  - `IPrintheadHal`
  - `PrintheadConfig`
  - `RasterMeta`
  - `MockPrintheadHal`
- `components/printer_hal/src/printhead_hal_mock.cpp`
  - 校验行长度
  - 模拟每行延迟
  - 支持 `fault_after_rows`
  - 统计行数和 RAW 字节数
- `components/app/src/print_service.cpp`
  - `os_print_row()` 将行同步提交给喷头 HAL
  - buffered path 也直接提交给喷头 HAL
  - RAW path 使用 `submitRaw()`
- `components/core/include/core/print_pipeline.h`
  - 网络、解压、顺序、CRC、背压和 Job 状态机与 HAL 解耦
- `components/printer_hal/include/printer_hal/stepper_hal.h`
  - 已有独立 `IStepperHal` / `MockStepperHal`
- `components/printer_hal/include/printer_hal/sensor_hal.h`
  - 已有可注入状态的 `MockSensorHal`

当前 Mock 的不足：

- `PrintService` 持有具体的 `MockPrintheadHal`，还没有正式的 HAL bundle/factory 注入点。
- 当前 Mock 只统计行数，不验证 CMYK 通道内容、CRC、点数、通道顺序或 row index。
- `RasterMeta` 只有 `row_index`，没有把物理 nozzle row、pass、feed position 暴露给上层。
- `PrintProfile::CMYK8` 只表达通用像素格式，不能表达 MH5440 的 `4 x 320` 喷嘴几何。
- 当前 `PrintService` 的 stepper 初始化和 `feed_paper` 路径是分开的，不能据此宣称已有真实 swath 同步。

因此，第一阶段不修改公共 IJPD 协议；先把 Mock 做成一个严格的逻辑验证器。

## 4. 建议的分层架构

```text
IJPD / MQTT / BLE / IPP
          |
          v
    PrintPipeline
          |
          v
    PrintService
          |
          v
   IPrintheadHal             IStepperHal / ISensorHal
          |                         |
          v                         v
 MockRicohMh5440Hal          MockPaperPath
          |
          +--> Mock metrics / CRC / trace
          +--> deterministic fault model
          +--> future RicohHeadPolicy
                                      |
                                      v
                           Future real controller HAL
                           GPIO/SPI/LCD_CAM/RMT only here
```

未来真实实现建议再拆一层：

```text
IPrintheadHal
    |
    +-- RicohMh5440PrintheadHal       # 行/通道/多 pass 策略
             |
             +-- IMh5440Controller    # 波形、总线、GPIO、电源、温控
```

`PrintService` 不应该知道 `IMh5440Controller`，也不应该知道 Ricoh 的寄存器或波形。

## 5. 第一阶段输入契约

### 5.1 逻辑输入格式

第一阶段 Mock 采用明确的、可测试的逻辑格式：

```text
PixelFormat: CMYK8
每个逻辑像素: [C, M, Y, K]，每通道 1 byte
row_bytes: logical_width_px * 4
row_index: 从 0 开始递增
行指针: 只在 submitRaster() 调用期间有效
```

这里的 `CMYK8` 是项目层输入格式，不是对 Ricoh 内部电气数据格式的声明。

### 5.2 MH5440 逻辑几何

Mock 默认记录以下几何：

```text
channels                 = 4
virtual_nozzles_per_ch   = 320
virtual_total_nozzles    = 1280
channel_pitch_dpi        = 150
head_native_reference_dpi = 600
```

`virtual_nozzles_per_ch=320` 不意味着输入一定必须是 320 像素宽。建议提供两种模式：

| 模式 | 行宽行为 | 用途 |
|---|---|---|
| `strict_head_geometry` | `row_bytes == 320 * 4` | 验证单个四色 head 的逻辑映射 |
| `logical_raster` | 使用 Job/Profile 的 `row_bytes`，只做 CMYK 统计 | 验证大图、协议和吞吐，不伪造物理映射 |

不要把 `total_nozzles=1280` 直接写成 `print_width_px=1280`。在当前 `CMYK8`
语义下，这会被解释为每个通道都有 1280 个逻辑像素，容易造成错误的行宽和能力上报。

### 5.3 A4 仅是整机纸路模型

单个 MH5440 的公开打印宽度是 `54.1 mm`，不是 A4 的 `210 mm`。因此：

- `A4` 可以作为 Mock 纸张和坐标系。
- 不能声称单个 MH5440 覆盖整张 A4。
- 如果以后要模拟 A4，需要明确选择 head array、横向扫描、拼接多 head 或缩放模型。
- 这些整机选择应放在 `MockPaperPath`/`MockPrinterModel`，不能隐藏在 `IPrintheadHal` 中。

## 6. Mock 行处理模型

`MockRicohMh5440Hal::submitRaster()` 建议按以下顺序处理：

1. 检查 HAL 已 `begin()` 且未 fault/stop。
2. 检查 `data != nullptr`、`len == configured_row_bytes`。
3. 检查 `row_index` 是否按策略递增；允许重复行必须显式配置。
4. 按 `[C,M,Y,K]` 拆分逻辑像素，但不修改输入缓存。
5. 计算每通道指标：
   - 输入字节 CRC32。
   - 非零像素数。
   - 总强度和最大强度。
   - 可选阈值后的 virtual dot count。
6. 应用确定性故障模型：fault-after-row、fault-after-byte、thermal fault、paper jam、backpressure。
7. 按模拟节拍调用注入的 `DelayFn`，主机测试默认不等待，设备 Mock 可使用 `vTaskDelay`。
8. 更新统计和 trace。
9. 返回 `Err::OK` 或明确的 `Err::HAL_FAULT` / `Err::INVALID_ARGUMENT`。

Mock 不应把每一行完整永久保存到 RAM。默认保存统计和 CRC；需要图像审计时使用有限容量的
trace sink 或外部文件流。

## 7. 建议 Mock 配置

Ricoh 专用配置不要塞进通用 `PrintheadConfig`。建议新增独立的 simulation 配置，或者
由 `MockRicohMh5440Hal` 构造函数注入：

```cpp
struct RicohMh5440MockConfig {
    uint32_t virtual_nozzles_per_channel = 320;
    uint8_t channel_count = 4;
    uint32_t logical_width_px = 320;
    uint32_t row_bytes = 320 * 4;

    bool strict_head_geometry = true;
    bool require_monotonic_rows = true;
    bool allow_raw = false;

    uint32_t row_period_us = 5000;
    uint32_t startup_delay_ms = 0;
    uint32_t stop_delay_ms = 0;

    uint32_t fault_after_rows = 0;
    uint64_t fault_after_bytes = 0;
    uint32_t backpressure_after_rows = 0;
    bool inject_thermal_fault = false;
    bool inject_paper_jam = false;

    // 仅用于确定性逻辑缺陷模拟，不代表真实喷嘴失效率。
    uint32_t missing_nozzle_ppm = 0;
    uint32_t random_seed = 0x54484D34u;
};
```

推荐的默认策略：

| 参数 | 默认 | 说明 |
|---|---:|---|
| `strict_head_geometry` | true | 早发现行宽与 head model 不一致 |
| `require_monotonic_rows` | true | 发现重复/跳行 |
| `row_period_us` | 5000 | 仅为仿真默认，不是厂商时序 |
| `missing_nozzle_ppm` | 0 | 正常回归必须无随机缺陷 |
| `random_seed` | 固定 | 相同输入必须得到相同 trace |
| `fault_after_rows` | 0 | 0 表示不注入 |
| `allow_raw` | false | MH5440 Mock 不接受 ESC/POS |

## 8. 状态和故障注入

建议 Mock 内部状态：

```text
Idle -> Ready -> Printing -> Idle
                 |  |
                 |  +-> Faulted
                 +----> Stopped
```

和现有 `IPrintheadHal` 的映射：

| 情况 | HAL 返回 | 上层预期 |
|---|---|---|
| 未 begin 就提交 | `INVALID_STATE` 或 `HAL_NOT_CONFIGURED` | 不应产生完成事件 |
| 行指针为空/长度错误 | `INVALID_ARGUMENT` | Job 失败，报告具体错误 |
| `fault_after_rows` 命中 | `HAL_FAULT` | PrintPipeline 进入 Failed |
| thermal fault | `HAL_FAULT` | 停止后续输出，保留 fault 原因 |
| paper jam | `HAL_FAULT` | 同时由 Sensor/PaperPath trace 记录 |
| `stop()` | `OK`，幂等 | 取消、超时和错误路径都可调用 |
| `waitUntilIdle()` 超时 | `QUEUE_TIMEOUT` 或 `HAL_FAULT` | 不得伪报完成 |
| `submitRaw()` | `UNSUPPORTED` | MH5440 raster-only Mock |

故障必须由配置和固定 seed 触发，不能使用不受控的随机数或实际延时作为故障来源。

## 9. Trace 和验收输出

每个 Mock Job 建议生成一个有限大小的 `MockJobStats`：

```cpp
struct MockJobStats {
    uint64_t rows_accepted = 0;
    uint64_t bytes_accepted = 0;
    uint64_t nonzero_pixels[4] = {};
    uint64_t intensity_sum[4] = {};
    uint32_t channel_crc32[4] = {};
    uint32_t first_bad_row = 0;
    uint32_t last_row_index = 0;
    uint32_t fault_count = 0;
    Err terminal_error = Err::OK;
};
```

验收时至少检查：

- 输入总字节数等于 `rows * row_bytes`。
- 每通道 CRC 可复现。
- 全白、全黑、单通道、棋盘格和渐变图的四通道统计符合预期。
- row index 严格递增，或明确记录允许的重复/跳行。
- `fault_after_rows=N` 在第 N 行产生 `HAL_FAULT`，且不会多接收后续行。
- cancel/timeout/断开后 `stop()` 可重复调用且不崩溃。
- Mock 报告的 bytes/CRC 与 `PrintService` 终态指纹一致。

建议不要把完整像素数组作为默认设备 trace；设备侧只保留计数、CRC 和首个错误位置，
主机测试可以使用内存 sink 保存小图样本。

## 10. 与当前代码的最小接入顺序

### 第一步：保持现有协议和 pipeline 不变

先使用现有：

- `PrintPipeline::Os::print_row`
- `PrintService::os_print_row`
- `IPrintheadHal::submitRaster`
- `MockStepperHal`
- `MockSensorHal`

新增 `MockRicohMh5440Hal` 时，先让它实现现有 `IPrintheadHal`。不在第一步修改 IPP、
IJPD 二进制协议或 `RasterMeta`。

当前已实现的代码入口：

- `components/printer_hal/include/printer_hal/ricoh_mh5440_mock_hal.h`
- `components/printer_hal/src/ricoh_mh5440_mock_hal.cpp`
- `profiles/ricoh_mh5440_mock.h`
- `host_tests/test_ricoh_mh5440_mock_hal.cpp`

`MockRicohMh5440Hal` 已覆盖 CMYK8 行校验、逐通道 CRC/统计、固定缺喷嘴模型、
行数/字节数故障、backpressure、纸张卡塞、过温、stop 幂等和 raster-only RAW 拒绝。

应用组装入口为 `PrinterApp::set_hal_bundle()`；该接口保存非 owning 指针，必须在
`PrinterApp::start()` 前调用，并保证具体 HAL 对象在整个服务生命周期内有效。没有注入
的成员会继续使用仓库内置 Mock。

### 第二步：把 PrintService 从具体 Mock 解耦

当前代码是具体成员：

```cpp
MockPrintheadHal printhead_{&mock_delay, nullptr};
```

建议改为 HAL bundle 注入：

```cpp
struct PrinterHalBundle {
    IPrintheadHal* printhead = nullptr;
    IStepperHal* stepper = nullptr;
    ICutterHal* cutter = nullptr;
    ISensorHal* sensors = nullptr;
};
```

`PrintService` 保存非 owning 指针；默认由 `PrinterApp` 组装 Mock bundle。未来真实版本只替换：

```text
MockRicohMh5440Hal  -> RicohMh5440PrintheadHal
MockStepperHal      -> BoardStepperHal
MockSensorHal       -> BoardSensorHal
MockCutterHal       -> BoardCutterHal（如存在）
```

`PrintPipeline`、控制协议、事件序列和上层 Job 代码不改。

默认 App 的屏幕预览也使用该 Profile 的几何：CMYK8 interleaved 行按 `320` 个逻辑
像素、4 通道解析，页面宽度按 `print_width_um=54100`（54.1 mm）计算。由于
`1280 bytes/row` 超过完整页缓存的预览阈值，设备采用 preview-only 模式实时渲染，
不会把 1280 个字节误当成 1280 个像素，也不会退化为错误的 A4 全宽预览。

### 第三步：再增加真实控制器层

拿到厂商资料后，按以下顺序实现：

1. `RicohMh5440Controller`：只负责电源、总线、GPIO、温度和波形发送。
2. `RicohMh5440PrintheadHal`：负责 row -> channel/nozzle/pass 调度。
3. `PrinterHalBundle`：注入真实 HAL。
4. 保留 `MockRicohMh5440Hal` 作为 host/实验室回归基线。

真实 HAL 通过 Mock 的同一组测试后，才允许打开真实打印能力。

## 11. Profile 建议

不要直接把当前 `profiles/inkjet_6in.h` 改成 MH5440。它是 `1800 px @ 300 dpi`
的通用 6-inch CMYK 示例，与单个 MH5440 的 `54.1 mm` head width 不是同一个物理模型。

当前已新增并由默认 `main/app_main.cpp` 注入：

```text
profiles/ricoh_mh5440_mock.h
```

Profile 只描述上层能力，例如：

```text
type              = inkjet
model_name        = RICOH-MH5440-MOCK
pixel_format      = cmyk8
data_modes        = raster
supports_lz4      = true
has_cutter        = false
has_auto_feed     = false，除非纸路 Mock 已配置
sensors           = 仅声明 Mock 实际实现的状态
```

`virtual_nozzles_per_channel`、`channel_pitch_dpi`、`paper_model` 和故障参数应属于
Mock/硬件配置，不应偷偷编码到通用 `PrintProfile` 的 `print_width_px`。

## 12. 主机测试矩阵

已实现以下两个不需要 ESP-IDF 的主机测试：

- `test_ricoh_mh5440_mock_hal`：Mock 本身的契约和故障模型。
- `test_ricoh_mh5440_pipeline`：真实 `PrintPipeline` 三泵到 Mock HAL 的集成路径。

建议继续维护以下测试矩阵：

| 类别 | 测试 |
|---|---|
| 初始化 | 合法配置、零宽度、零通道、非法 row bytes、重复 begin |
| 行输入 | 正确 CMYK8 行、空指针、短行、长行、row index 跳跃/重复 |
| 通道统计 | 全白、全黑、单 C/M/Y/K、交错像素、固定渐变 |
| 指纹 | 总 CRC、每通道 CRC、重复运行结果一致 |
| 节拍 | delay callback 收到预期次数和时长；主机默认不 sleep |
| 故障 | after-row、after-byte、thermal、jam、backpressure |
| 生命周期 | begin -> submit -> wait -> stop；stop 幂等；fault 后 stop 安全 |
| 上层集成 | PrintPipeline 完成、取消、HAL_FAULT、槽位回收和终态指纹 |
| 资源 | 不复制整页；固定容量 trace 超限时明确丢弃/报错 |

建议的小图样本：

```text
1. 4x4 全白
2. 4x4 全黑
3. C/M/Y/K 各一列
4. 每个通道独立渐变
5. 交错 checkerboard
6. 末行 fault-after-row
```

## 13. 未来真实硬件接入清单

在实现 `RicohMh5440PrintheadHal` 前，必须取得并归档：

- 驱动板原理图、MCU 接口、电平和连接器定义。
- 喷头供电、加热器供电、限流、上电/掉电顺序。
- 控制协议或并行时序图、clock/latch/strobe 关系。
- binary 和 grayscale 的完整波形参数。
- 4 色通道顺序、320 nozzle 行映射、交错补偿和数据位序。
- 温度读取、heater 控制、过温阈值和故障恢复。
- 走纸距离、head-to-paper 几何、首行偏移、pass/swash 策略。
- 维护喷射、清洗、盖帽和长期 idle 行为。

缺少其中任一项时，继续使用 Mock；不要通过增加 delay 或 GPIO 猜测替代资料。

## 14. 当前结论

现在最合适的方案是：

1. 用现有 `IPrintheadHal` 作为稳定上层边界。
2. 增加 Ricoh-specific 的逻辑 Mock，而不是伪造真实电气驱动。
3. Mock 输入采用显式 `CMYK8` 行契约，默认严格校验 `4 x 320` 的 virtual head geometry。
4. 用每通道 CRC、点数、强度、行号和确定性 fault trace 验证行为。
5. 把纸路、扫描/多 pass 和真实控制器放在独立层。
6. 未来只替换 HAL bundle 和真实 controller，不修改网络协议、PrintPipeline 或 Job 状态机。

这能在没有真实喷头时先完成软件、数据和故障路径验证，同时避免把未经确认的 MH5440
电气细节固化进固件。
