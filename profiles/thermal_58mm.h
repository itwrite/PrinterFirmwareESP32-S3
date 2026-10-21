// ============================================================================
// profiles/thermal_58mm.h —— 58mm 热敏打印机 Profile 示例（中间件 Phase M1）
//
// 典型配置：384 点热敏头（58mm / 203dpi）、1bit/像素、ESC/POS + 栅格双模式、
// 缺纸/开盖传感器、自动切刀。
// 集成：在 main 中 #include 并传给 PrinterApp::set_profile()。
// ============================================================================
#ifndef IJPD_PROFILES_THERMAL_58MM_H
#define IJPD_PROFILES_THERMAL_58MM_H

#include "core/print_profile.h"

namespace ijpd {
namespace profiles {

inline constexpr PrintProfile kThermal58mm = {
    // 身份
    .type = PrinterType::Thermal,
    .type_name = "thermal",
    .model_name = "TP-58",

    // 几何/像素：384 点 × 1bit = 48 字节/行
    .pixel_format = PixelFormat::Mono1,
    .pixel_format_name = "mono1",
    .print_width_px = 384,
    .dpi = 203,
    .bytes_per_row_min = 48,
    .bytes_per_row_max = 48,        // 固定行宽

    // 数据模式
    .data_modes = DataMode::Raster | DataMode::EscPos,
    .escpos_chunk_bytes = 512,

    // 压缩
    .supports_lz4 = true,

    // 硬件能力
    .has_cutter = true,
    .has_auto_feed = true,
    .sensors = SensorFlags::PaperOut | SensorFlags::CoverOpen | SensorFlags::Overtemp,
};

}  // namespace profiles
}  // namespace ijpd

#endif  // IJPD_PROFILES_THERMAL_58MM_H
