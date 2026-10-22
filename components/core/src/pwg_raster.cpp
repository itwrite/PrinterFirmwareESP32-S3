// pwg_raster.cpp - Bounded PWG Raster v2 page-header parser.
#include "core/pwg_raster.h"

#include <cstring>

namespace ijpd {
namespace {

uint32_t be32(const uint8_t* p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

// Offsets are relative to the start of cups_page_header2_t, immediately after
// the four-byte PWG sync word. All PWG Raster v2 integer fields are big-endian.
inline constexpr size_t kWidth = 372;
inline constexpr size_t kHeight = 376;
inline constexpr size_t kDpiX = 276;
inline constexpr size_t kDpiY = 280;
inline constexpr size_t kBitsPerColor = 384;
inline constexpr size_t kBitsPerPixel = 388;
inline constexpr size_t kBytesPerLine = 392;
inline constexpr size_t kColorOrder = 396;
inline constexpr size_t kColorSpace = 400;
inline constexpr size_t kCompression = 404;

}  // namespace

Err parse_pwg_raster_header(const uint8_t* data, size_t len,
                            PwgRasterPage* out) noexcept {
    if (data == nullptr || out == nullptr || len < PWG_RASTER_HEADER_BYTES) {
        return Err::INVALID_ARGUMENT;
    }
    if (std::memcmp(data, "RaS2", PWG_RASTER_SYNC_BYTES) != 0) {
        return Err::INVALID_ARGUMENT;
    }

    const uint8_t* h = data + PWG_RASTER_SYNC_BYTES;
    PwgRasterPage parsed{};
    parsed.width = be32(h + kWidth);
    parsed.height = be32(h + kHeight);
    parsed.dpi_x = be32(h + kDpiX);
    parsed.dpi_y = be32(h + kDpiY);
    parsed.bits_per_color = be32(h + kBitsPerColor);
    parsed.bits_per_pixel = be32(h + kBitsPerPixel);
    parsed.bytes_per_line = be32(h + kBytesPerLine);
    parsed.color_order = be32(h + kColorOrder);
    parsed.color_space = be32(h + kColorSpace);
    parsed.compression = be32(h + kCompression);

    if (parsed.width == 0 || parsed.height == 0 || parsed.dpi_x == 0 ||
        parsed.dpi_y == 0 || parsed.bits_per_color == 0 ||
        parsed.bits_per_color > 8 || parsed.bits_per_pixel == 0 ||
        parsed.bits_per_pixel > 32 || parsed.bytes_per_line == 0) {
        return Err::INVALID_ARGUMENT;
    }
    if ((parsed.bits_per_pixel % parsed.bits_per_color) != 0 ||
        parsed.bits_per_pixel < parsed.bits_per_color) {
        return Err::INVALID_ARGUMENT;
    }
    if (parsed.compression != 0) return Err::UNSUPPORTED;

    const uint64_t total = static_cast<uint64_t>(parsed.bytes_per_line) * parsed.height;
    if (total == 0 || total > cfg::MAX_JOB_BYTES ||
        parsed.bytes_per_line > cfg::MAX_PACKET_UNCOMPRESSED_SIZE) {
        return Err::INVALID_ARGUMENT;
    }
    const uint64_t minimum_line =
        (static_cast<uint64_t>(parsed.width) * parsed.bits_per_pixel + 7) / 8;
    if (parsed.bytes_per_line < minimum_line) return Err::INVALID_ARGUMENT;

    parsed.total_bytes = static_cast<uint32_t>(total);
    *out = parsed;
    return Err::OK;
}

}  // namespace ijpd
