// ============================================================================
// profiles/inkjet_6in.h —— 6 英寸彩色喷墨照片打印机 Profile 示例
// ============================================================================
#ifndef IJPD_PROFILES_INKJET_6IN_H
#define IJPD_PROFILES_INKJET_6IN_H

#include "core/print_profile.h"

namespace ijpd {
namespace profiles {

inline constexpr PrintProfile kInkjet6in = {
    // 身份
    .type = PrinterType::Inkjet,
    .type_name = "inkjet",
    .model_name = "IJPD-P61",

    // 几何/像素：6 寸 @ 300dpi（喷头未确认，待 Phase 7 填入真实值）
    .pixel_format = PixelFormat::CMYK8,
    .pixel_format_name = "cmyk8",
    .print_width_px = 1800,         // 6in × 300dpi
    .dpi = 300,
    .bytes_per_row_min = 1,
    .bytes_per_row_max = 0,         // 由 print_width_px × bpp 推导

    // 数据模式
    .data_modes = DataMode::Raster, // 喷墨不走 ESC/POS
    .escpos_chunk_bytes = 0,

    // 压缩
    .supports_lz4 = true,

    // 硬件能力（喷头未确认前全 false）
    .has_cutter = false,
    .has_auto_feed = false,
    .sensors = SensorFlags::None,
};

}  // namespace profiles
}  // namespace ijpd

#endif  // IJPD_PROFILES_INKJET_6IN_H
