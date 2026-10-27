// ricoh_mh5440_mock.h - Logical four-pass CMYK1 profile for the MH5440 mock.
#ifndef IJPD_PROFILES_RICOH_MH5440_MOCK_H
#define IJPD_PROFILES_RICOH_MH5440_MOCK_H

#include "core/print_profile.h"

namespace ijpd {
namespace profiles {

// Four 1-bit planes are sent in C/M/Y/K order and printed as four passes.
// This software-only Mock uses an A4 logical raster at 300 DPI; it does not
// claim that a physical MH5440 head can cover an A4 sheet.
inline constexpr PrintProfile kRicohMh5440Mock = {
    .type = PrinterType::Inkjet,
    .type_name = "inkjet",
    .model_name = "RICOH-MH5440-MOCK",
    .pixel_format = PixelFormat::CMYK1,
    .pixel_format_name = "cmyk1",
    .print_width_px = 2480,
    .print_height_px = 3508,
    .print_width_um = 210000,
    .dpi = 300,
    .bytes_per_row_min = 310,
    .bytes_per_row_max = 310,
    .data_modes = DataMode::Raster,
    .escpos_chunk_bytes = 0,
    .supports_lz4 = true,
    .has_cutter = false,
    .has_auto_feed = false,
    .sensors = SensorFlags::None,
    .raster_layout = RasterLayout::Planar,
    .channel_count = 4,
    .channel_order = {RasterChannel::Cyan, RasterChannel::Magenta,
                      RasterChannel::Yellow, RasterChannel::Black},
    .print_channels_sequentially = true,
};

}  // namespace profiles
}  // namespace ijpd

#endif  // IJPD_PROFILES_RICOH_MH5440_MOCK_H
