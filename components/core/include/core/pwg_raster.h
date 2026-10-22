// ============================================================================
// pwg_raster.h - Bounded PWG Raster v2 page-header parser.
//
// A PWG Raster page is:
//   4-byte sync "RaS2" + 1796-byte cups_page_header2_t + row data.
// This parser validates the fixed header and exposes the row layout. It does
// not allocate memory, decode compression, or accept arbitrary document data.
// ============================================================================
#ifndef IJPD_CORE_PWG_RASTER_H
#define IJPD_CORE_PWG_RASTER_H

#include <cstddef>
#include <cstdint>

#include "core/config_limits.h"
#include "core/errors.h"

namespace ijpd {

inline constexpr size_t PWG_RASTER_SYNC_BYTES = 4;
inline constexpr size_t PWG_RASTER_PAGE_HEADER_BYTES = 1796;
inline constexpr size_t PWG_RASTER_HEADER_BYTES =
    PWG_RASTER_SYNC_BYTES + PWG_RASTER_PAGE_HEADER_BYTES;

struct PwgRasterPage {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t dpi_x = 0;
    uint32_t dpi_y = 0;
    uint32_t bits_per_color = 0;
    uint32_t bits_per_pixel = 0;
    uint32_t bytes_per_line = 0;
    uint32_t color_space = 0;
    uint32_t color_order = 0;
    uint32_t compression = 0;
    uint32_t total_bytes = 0;
};

// Parse and validate one PWG Raster v2 page header. The input must contain at
// least the fixed 1800-byte header; row data is not required by this function.
Err parse_pwg_raster_header(const uint8_t* data, size_t len,
                            PwgRasterPage* out) noexcept;

}  // namespace ijpd

#endif  // IJPD_CORE_PWG_RASTER_H
