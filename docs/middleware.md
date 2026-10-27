# IJPD 厂商接入指南

本文面向需要把另一台打印机接入 IJPD 固件的开发者。第三方 App 协议请看
`protocols/README.md`；本文只说明固件变体、Profile 和 HAL 的工程接入。

## 1. 固件分层

```text
main/app_main.cpp
    PrinterConfig
        PrintProfile  -> 打印机能力、几何、像素格式、数据模式
        service flags  -> Wi-Fi / BLE / IJPD TCP / RAW TCP / MQTT
    PrinterApp
        ControlPlane / PrintService
    printer_hal
        Printhead / Stepper / Cutter / Sensor HAL
```

`core/` 不依赖 ESP-IDF；`platform_esp/` 负责网络、BLE、任务和 PSRAM；
`app/` 负责组装控制面和打印服务；`printer_hal/` 只提供硬件接口与 Mock。

## 2. 定义 Profile

在 `profiles/` 新增静态 Profile：

```cpp
#include "core/print_profile.h"

inline constexpr ijpd::PrintProfile kMyThermal = {
    .type = ijpd::PrinterType::Thermal,
    .type_name = "thermal",
    .model_name = "MY-TP80",
    .pixel_format = ijpd::PixelFormat::Mono1,
    .pixel_format_name = "mono1",
    .print_width_px = 576,
    .dpi = 203,
    .bytes_per_row_min = 72,
    .bytes_per_row_max = 72,
    .data_modes = ijpd::DataMode::Raster,
    .supports_lz4 = true,
    .has_cutter = false,
    .has_auto_feed = true,
    .sensors = ijpd::SensorFlags::PaperOut | ijpd::SensorFlags::CoverOpen,
};
```

关键规则：

- `Mono1` 的一行字节数按 `(print_width_px + 7) / 8` 计算。
- `bytes_per_row_min/max` 控制 `start_print` 的行宽校验。
- `DataMode::Raster` 表示结构化行数据。
- `DataMode::EscPos` 只表示 Profile 声明兼容 ESC/POS；当前 IJPD
  `start_print.mode="escpos"` 路径仍未完成，原始流应使用独立 RAW TCP 服务。
- `supports_lz4` 控制 Profile 是否允许 `compression:"lz4_block"`。
- `has_cutter` 会影响能力上报和 `cut_paper` 门控，但真实切刀仍需 HAL。
- 未确认的喷头、传感器和机械能力应保持 `false` 或 Mock，不应编造规格。

## 3. 配置固件变体

在 `main/app_main.cpp` 注入配置：

```cpp
#include "profiles/my_thermal.h"

static constexpr ijpd::PrinterConfig kVariant = {
    .profile = &kMyThermal,
    .services = {
        .wifi = true,
        .ble = true,
        .ijpd_tcp = true,
        .escpos_raw_tcp = false,
        .mqtt = false,
    },
    .device_model = "MY-TP80",
    .fw_version = "0.4.0",
    .ble_name = "MY-TP80",
    .escpos_raw_tcp_port = 9102,
    .mqtt_uri = nullptr,
    .mqtt_base_topic = nullptr,
};
```

默认固件当前为：

```text
Wi-Fi + BLE + IJPD TCP: enabled
RAW TCP:                disabled
MQTT:                   disabled
Profile:                nullptr
```

如果开启 RAW TCP，必须同时提供 `DataMode::EscPos` Profile；没有 Profile 或
Profile 不支持 ESC/POS 时，`PrinterApp` 会自动关闭该服务。

如果开启 MQTT，必须提供有效的 `mqtt_uri` 和 `mqtt_base_topic`。MQTT 的外部
topic 和 IJCH 格式见 `protocols/mqtt.md`。

## 4. 实现 HAL

### Printhead

实现 `IPrintheadHal` 的核心方法：

```cpp
Err begin(const PrintheadConfig& cfg);
Err submitRaster(const uint8_t* row, size_t len, const RasterMeta& meta);
Err submitRaw(const uint8_t* data, size_t len);
Err waitUntilIdle(uint32_t timeout_ms);
Err stop();
```

在获得官方时序、电压、喷嘴映射和温度窗口前，不要实现真实点火路径。当前
固件的默认实现是 Mock HAL。

RICOH MH5440 没有真实硬件时的逻辑 Mock 方案见
[`RICOH_MH5440_Mock_HAL.md`](../RICOH_MH5440_Mock_HAL.md)。该方案明确区分公开
喷头规格、整机纸路仿真和未来控制器电气层，不把 A4 logical Mock 或仿真 DPI 误报为单个
MH5440 的物理覆盖能力。

### Stepper / Cutter / Sensor

分别实现 `IStepperHal`、`ICutterHal`、`ISensorHal`。真实硬件接入至少需要：

- 电机步距、微步、传动比、速度和加减速限制。
- 切刀驱动、电气限位和安全联锁。
- 缺纸、开盖、过温、卡纸的电平和去抖规则。

在参数闭合前，保留 Mock 或 `UNSUPPORTED`，不要通过延时猜测机械位置。

## 5. Profile 与 App 对接检查

固件启动后用 `get_device_info` 检查：

- `printhead.dpi`
- `printhead.print_width_px`
- `printhead.pixel_format`
- `capabilities.data_modes`
- `capabilities.compressions`
- `capabilities.services`
- `capabilities.service_ports`
- `capabilities.cutter`
- `capabilities.sensors`

第三方 App 不应只看 `device.connections`；实际开关以
`capabilities.services` 为准。

## 6. 验证清单

```bash
# 主机单元测试
cmake -S host_tests -B host_tests/build
cmake --build host_tests/build
ctest --test-dir host_tests/build --output-on-failure

# 固件构建
tools/idf.sh -B build-default build

# 设备联调
python3 tools/send_job.py --host <IP> --bytes 120000 --packets 30 --pattern mixed
```

新增 Profile 或 HAL 时至少覆盖：

- `get_device_info` 能力和 Profile 字段。
- raster 行宽、总行数、总字节数校验。
- 无压缩、LZ4 存储兜底、LZ4 压缩、CRC32。
- 取消、断开、空闲超时和总超时。
- HAL 故障和任务终态事件。
- PSRAM 槽位回收和重复任务无泄漏。

## 7. 默认固件当前明确不支持

- 默认 app 集成的 IPP/AirPrint 打印链路。仓库中的 opt-in AirPrint Core/transport
  已有经真实样本验证的 host Apple Raster Mono8/SW decoder 子集，但仍缺少默认
  app 集成和硬件 backend bridge。
- PDF、Apple Raster 等文档转换。
- 默认镜像的 IJPD `start_print.mode="escpos"` 完整数据路径。
- 未提供硬件资料时的真实喷头点火。
