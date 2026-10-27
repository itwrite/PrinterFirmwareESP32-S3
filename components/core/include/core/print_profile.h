// ============================================================================
// print_profile.h —— 打印机 Profile（中间件插件接口，Phase M1）
//
// 每种打印机（热敏/喷墨/标签…）提供一个静态 PrintProfile 实例，
// 描述其数据格式与能力。中间件据此驱动：
//   - 命令校验（start_print mode / bytes_per_row 范围 / 压缩选项）
//   - device_info 上报（printer_type / pixel_format / data_modes）
//   - 流水线行为（raster 行重组 vs escpos 原始流转发）
//
// 集成方式：打印机厂商在 main/ 中定义 const PrintProfile，经
// PrinterApp::set_profile() 注入。
//
// 线程安全性：纯只读静态数据（init 后不再修改）。
// ============================================================================
#ifndef IJPD_CORE_PRINT_PROFILE_H
#define IJPD_CORE_PRINT_PROFILE_H

#include <cstddef>
#include <cstdint>

#include "core/config_limits.h"
#include "core/errors.h"
#include "core/raster_channels.h"

namespace ijpd {

// ---- 打印机类型 ----
enum class PrinterType : uint8_t {
    Thermal = 0,   // 热敏（收据/标签）
    Inkjet = 1,    // 喷墨（照片/文档）
    Label = 2,     // 标签（热转印/热敏）
    DotMatrix = 3, // 针式
    Unknown = 0xFF,
};

// ---- 像素格式 ----
enum class PixelFormat : uint8_t {
    Mono1 = 0,    // 1 bit/px（热敏最常见）
    Mono8 = 1,    // 灰度 8 bit/px
    RGB565 = 2,   // 16 bit/px
    RGB888 = 3,   // 24 bit/px
    CMYK8 = 4,    // 32 bit/px（喷墨）
    CMYK1 = 5,    // 四个独立 1-bit 平面，依次 C/M/Y/K 多遍打印
    Unknown = 0xFF,
};

enum class RasterLayout : uint8_t {
    Planar = 0,       // 每个通道为独立平面，按 channel_order 顺序排列
    Interleaved = 1,  // 同一像素各通道交错存储
    Unknown = 0xFF,
};

// ---- 数据模式（位掩码，可组合） ----
enum class DataMode : uint8_t {
    None = 0,
    Raster = 0x01,   // 结构化栅格行（bytes_per_row × total_rows）
    EscPos = 0x02,   // ESC/POS 原始流透传（total_bytes，无行结构）
};

inline constexpr DataMode operator|(DataMode a, DataMode b) noexcept {
    return static_cast<DataMode>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}
inline constexpr bool has_mode(DataMode modes, DataMode m) noexcept {
    return (static_cast<uint8_t>(modes) & static_cast<uint8_t>(m)) != 0;
}

// ---- 传感器（位掩码） ----
enum class SensorFlags : uint8_t {
    None = 0,
    PaperOut = 0x01,   // 缺纸
    CoverOpen = 0x02,  // 开盖
    Overtemp = 0x04,   // 过温
    PaperJam = 0x08,   // 卡纸
};

inline constexpr SensorFlags operator|(SensorFlags a, SensorFlags b) noexcept {
    return static_cast<SensorFlags>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}
inline constexpr bool has_sensor(SensorFlags s, SensorFlags m) noexcept {
    return (static_cast<uint8_t>(s) & static_cast<uint8_t>(m)) != 0;
}

// ---- Profile 结构 ----
struct PrintProfile {
    // 身份
    PrinterType type = PrinterType::Unknown;
    const char* type_name = "unknown";   // 协议上报名（"thermal"/"inkjet"/...）
    const char* model_name = "";          // 厂商型号

    // 几何/像素
    PixelFormat pixel_format = PixelFormat::Mono1;
    const char* pixel_format_name = "mono1";
    uint16_t print_width_px = 0;          // 最大打印宽度（像素）
    uint32_t print_height_px = 0;         // Maximum native raster height; zero means unspecified.
    uint32_t print_width_um = 0;           // 喷头/打印区域逻辑宽度（微米，0=未知）
    uint16_t dpi = 0;
    uint16_t bytes_per_row_min = 1;       // 行字节数下限（0 = 不限）
    uint16_t bytes_per_row_max = 0;       // 行字节数上限（0 = 由 print_width_px 推导）

    // 数据模式
    DataMode data_modes = DataMode::Raster;
    uint16_t escpos_chunk_bytes = 512;    // escpos 模式每次转发给 HAL 的块大小

    // 压缩（协议 compression 枚举按此过滤）
    bool supports_lz4 = true;

    // 硬件能力
    bool has_cutter = false;
    bool has_auto_feed = false;
    SensorFlags sensors = SensorFlags::None;

    // Raster contract and print-head scheduling, independent of model name.
    RasterLayout raster_layout = RasterLayout::Planar;
    uint8_t channel_count = 1;
    RasterChannel channel_order[4] = {RasterChannel::Black};
    bool print_channels_sequentially = false;

    uint8_t bits_per_channel() const noexcept {
        switch (pixel_format) {
            case PixelFormat::Mono1:
            case PixelFormat::CMYK1: return 1;
            case PixelFormat::Mono8:
            case PixelFormat::CMYK8: return 8;
            default: return 0;
        }
    }

    uint32_t expected_bytes_per_row() const noexcept {
        const uint32_t channel_bits = bits_per_channel();
        if (channel_bits == 0 || channel_count == 0) return 0;
        if (raster_layout == RasterLayout::Interleaved) {
            return (static_cast<uint32_t>(print_width_px) * channel_bits * channel_count + 7) / 8;
        }
        return (static_cast<uint32_t>(print_width_px) * channel_bits + 7) / 8;
    }

    uint32_t expected_bytes_per_pixel() const noexcept {
        if (raster_layout != RasterLayout::Interleaved) return 0;
        const uint32_t bits = static_cast<uint32_t>(bits_per_channel()) * channel_count;
        return (bits + 7) / 8;
    }

    uint32_t expected_total_rows() const noexcept {
        if (print_height_px == 0) return 0;
        return print_height_px *
            (raster_layout == RasterLayout::Planar ? channel_count : 1);
    }

    // ---- 校验 ----
    // Profile 自身合法性（init 时调用一次）
    Err validate() const noexcept {
        if (type == PrinterType::Unknown || type_name == nullptr ||
            type_name[0] == '\0') {
            return Err::INVALID_ARGUMENT;
        }
        if (print_width_px == 0 || dpi == 0) {
            return Err::INVALID_ARGUMENT;
        }
        if (static_cast<uint8_t>(data_modes) == 0) {
            return Err::INVALID_ARGUMENT;  // 至少支持一种数据模式
        }
        if (bytes_per_row_min == 0) {
            return Err::INVALID_ARGUMENT;
        }
        return Err::OK;
    }

    // bytes_per_row 上限（显式值或由宽度推导）
    uint32_t effective_bpr_max(uint8_t bytes_per_pixel) const noexcept {
        if (bytes_per_row_max != 0) return bytes_per_row_max;
        const uint32_t expected = expected_bytes_per_row();
        if (expected != 0) return expected;
        if (bytes_per_pixel == 0) return cfg::MAX_PACKET_UNCOMPRESSED_SIZE;
        return static_cast<uint32_t>(print_width_px) * bytes_per_pixel;
    }

    // 像素格式的每像素字节数
    uint8_t bytes_per_pixel() const noexcept {
        switch (pixel_format) {
            case PixelFormat::Mono1:  return 1;  // 特殊：按位打包，行宽=px/8
            case PixelFormat::Mono8:  return 1;
            case PixelFormat::RGB565: return 2;
            case PixelFormat::RGB888: return 3;
            case PixelFormat::CMYK8:  return 4;
            case PixelFormat::CMYK1:  return 0;  // 平面位图，按每行位打包
            default:                  return 0;
        }
    }

    // Mono1 的行字节数（8 像素/字节，向上取整）
    uint32_t mono1_row_bytes() const noexcept {
        return (static_cast<uint32_t>(print_width_px) + 7) / 8;
    }

    const char* printer_type_name(PrinterType t) const noexcept {
        switch (t) {
            case PrinterType::Thermal:   return "thermal";
            case PrinterType::Inkjet:    return "inkjet";
            case PrinterType::Label:     return "label";
            case PrinterType::DotMatrix: return "dot_matrix";
            default:                     return "unknown";
        }
    }
};

// pixel_format 名 -> 枚举（解析协议 JSON 用）
inline PixelFormat pixel_format_from_name(const char* name, size_t len) noexcept {
    if (len == 5 && __builtin_memcmp(name, "mono1", 5) == 0) return PixelFormat::Mono1;
    if (len == 5 && __builtin_memcmp(name, "mono8", 5) == 0) return PixelFormat::Mono8;
    if (len == 5 && __builtin_memcmp(name, "rgb565", 5) == 0) return PixelFormat::RGB565;
    if (len == 6 && __builtin_memcmp(name, "rgb888", 6) == 0) return PixelFormat::RGB888;
    if (len == 5 && __builtin_memcmp(name, "cmyk8", 5) == 0) return PixelFormat::CMYK8;
    if (len == 5 && __builtin_memcmp(name, "cmyk1", 5) == 0) return PixelFormat::CMYK1;
    return PixelFormat::Unknown;
}

}  // namespace ijpd

#endif  // IJPD_CORE_PRINT_PROFILE_H
