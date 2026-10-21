# IJPD 通用打印机协议中间件

> 把你的打印机接入统一 App 生态：App 只需实现一次协议客户端，即可控制任何支持本中间件的打印机（热敏/喷墨/标签/针式）。

## 架构

```
┌────────────────────────────────────────────────────┐
│          Your App（iOS/Android/PC/Web）             │
│    统一协议客户端：JSON 控制 + 二进制数据            │
├────────────────────────────────────────────────────┤
│                                                    │
│  IJPD 中间件（本工程，编译到打印机固件中）           │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ │
│  传输层   TCP 9100（控制）· TCP 9101（数据）        │
│           BLE GATT（控制，分片重组）                │
│  控制层   JSON + '#' 拆帧 · 命令路由 · 事件广播     │
│  数据层   Hello 握手 · LZ4/CRC32 分包               │
│           槽位池 · 流控（高低水位背压）              │
│           流水线（raster 行重组 / escpos 原始流）    │
│  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ │
│                                                    │
├────────────────────────────────────────────────────┤
│  🔌 PrintProfile（每种打印机一个静态实例）          │
│     printer_type / pixel_format / data_modes       │
│     print_width / dpi / sensors / capabilities     │
├────────────────────────────────────────────────────┤
│  🔌 Printer HAL（每种打印机实现这组接口）           │
│     IPrintheadHal（submitRaster + submitRaw）       │
│     IStepperHal · ICutterHal · ISensorHal          │
├────────────────────────────────────────────────────┤
│                    Your Hardware                    │
└────────────────────────────────────────────────────┘
```

## 集成指南（5 步接入你的打印机）

### 第 1 步：定义 Profile

```cpp
// profiles/my_printer.h
#include "core/print_profile.h"

inline constexpr ijpd::PrintProfile kMyPrinter = {
    .type = ijpd::PrinterType::Thermal,
    .type_name = "thermal",
    .model_name = "MY-TP80",
    .pixel_format = ijpd::PixelFormat::Mono1,
    .pixel_format_name = "mono1",
    .print_width_px = 576,           // 80mm @ 203dpi
    .dpi = 203,
    .bytes_per_row_min = 72,          // 576/8
    .bytes_per_row_max = 72,
    .data_modes = ijpd::DataMode::Raster | ijpd::DataMode::EscPos,
    .supports_lz4 = true,
    .has_cutter = true,
    .sensors = ijpd::SensorFlags::PaperOut | ijpd::SensorFlags::CoverOpen,
};
```

### 第 2 步：实现 HAL

```cpp
// my_hal.cpp
class MyThermalHead : public ijpd::IPrintheadHal {
    Err begin(const PrintheadConfig& cfg) override { /* 初始化你的热敏头 */ }
    Err submitRaster(const uint8_t* row, size_t len, const RasterMeta& m) override {
        /* 把一行像素写入热敏头并触发加热 */
    }
    Err submitRaw(const uint8_t* data, size_t len) override {
        /* ESC/POS 解释器或直接转发到已有固件 */
    }
    Err waitUntilIdle(uint32_t ms) override { /* 等待打印完成 */ }
    Err stop() override { /* 安全停机 */ }
    bool healthy() const override { /* 温度/状态检查 */ }
};

class MySensor : public ijpd::ISensorHal {
    bool get(SensorId id) const override { /* 读 GPIO / ADC */ }
    SensorFlags supported() const override { return kMyPrinter.sensors; }
};
```

### 第 3 步：main 中组装

```cpp
// main/app_main.cpp
#include "app/printer_app.h"
#include "profiles/my_printer.h"

extern "C" void app_main() {
    auto& app = ijpd::PrinterApp::instance();
    app.set_profile(&ijpd::kMyPrinter);     // 注入 Profile
    app.start();                             // 启动中间件
    // HAL 实现注入（Phase 7 硬件接入后）
    // app.print_service().set_printhead(new MyThermalHead());
    // app.print_service().set_sensor(new MySensor());
}
```

### 第 4 步：App 侧统一调用

```
1. get_device_info → 读取 printer_type / data_modes / pixel_format / sensors
2. start_print(mode="raster", bytes_per_row=72, ...)  → 结构化栅格
   start_print(mode="escpos", ...)                    → ESC/POS 原始流
3. 数据通道发送 → 中间件自动校验/解压/排队/转发
4. 接收 print_progress / print_finished / sensor_alert 事件
```

### 第 5 步：验证

```bash
# 主机模拟客户端
python3 tools/send_job.py --host <IP> --pattern mixed
python3 tools/send_job.py --host <IP> --corrupt-crc     # 异常注入
```

## 中间件为你解决什么

| 层 | 问题 | 中间件方案 |
|---|---|---|
| **传输** | 每种打印机用不同接口（USB/SERIAL/BLE/WiFi） | 统一 TCP + BLE，端口固定 9100/9101 |
| **控制** | 每家私有命令集 | JSON + `#` 拆帧，6 标准命令 + 事件 |
| **数据** | 裸 TCP 丢包/粘包/校验 | Hello 令牌握手 + LZ4 压缩 + CRC32 + 严格顺序 |
| **流控** | 打印机内存有限，大文件溢出 | PSRAM 槽位池 + 高低水位 TCP 背压 |
| **状态** | 各家状态上报格式不同 | 统一 11 态状态机 + 事件广播 |
| **多类型** | 热敏/喷墨/标签数据格式不同 | Profile 声明 + raster/escpos 双模式 |

## App 侧优势

一次开发，全类型覆盖：

```
┌──────────┐     ┌──────────────────────────────────┐
│  Your App │────▶│ get_device_info                  │
│  (统一)   │     │ → printer_type: "thermal"        │
│           │     │ → pixel_format: "mono1"          │
│           │     │ → data_modes: ["raster","escpos"]│
│           │     │ → sensors: ["paper_out",...]     │
│           │     └──────────────────────────────────┘
│           │     App 据此适配 UI 与数据生成策略：
│           │     · 热敏 → 位图光栅化 → raster 模式
│           │     · 喷墨 → CMYK 排版  → raster 模式
│           │     · 遗留 → ESC/POS 流 → escpos 模式
└──────────┘
```

## 支持的打印机类型

| 类型 | PixelFormat | DataMode | 典型场景 |
|---|---|---|---|
| 热敏 | Mono1 | Raster + EscPos | 收据/小票/标签 |
| 喷墨 | CMYK8/RGB888 | Raster | 照片/文档 |
| 标签 | Mono1 | Raster | 物流/条码 |
| 针式 | Mono1 | Raster + EscPos | 票据/多层复写 |

新增类型只需在 `PrinterType` 枚举加值 + 定义 Profile。

## 协议版本

- **v1.0**（Phase 0 定稿）：基础协议
- **v1.1**（中间件化）：新增 `mode` 参数（raster/escpos）、`printer_type`/`data_modes`/`sensors` 字段
