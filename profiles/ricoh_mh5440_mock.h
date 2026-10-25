// ricoh_mh5440_mock.h - Logical CMYK8 profile for the MH5440 mock.
#ifndef IJPD_PROFILES_RICOH_MH5440_MOCK_H
#define IJPD_PROFILES_RICOH_MH5440_MOCK_H

#include "core/print_profile.h"

namespace ijpd {
namespace profiles {

// One logical pixel contains C/M/Y/K bytes. The 320-pixel width models one
// four-color virtual head row; it is not an A4 page width claim.
inline constexpr PrintProfile kRicohMh5440Mock = {
    .type = PrinterType::Inkjet,
    .type_name = "inkjet",
    .model_name = "RICOH-MH5440-MOCK",
    .pixel_format = PixelFormat::CMYK8,
    .pixel_format_name = "cmyk8",
    .print_width_px = 320,
    .print_width_um = 54100,
    .dpi = 600,
    .bytes_per_row_min = 1280,
    .bytes_per_row_max = 1280,
    .data_modes = DataMode::Raster,
    .escpos_chunk_bytes = 0,
    .supports_lz4 = true,
    .has_cutter = false,
    .has_auto_feed = false,
    .sensors = SensorFlags::None,
};

}  // namespace profiles
}  // namespace ijpd

#endif  // IJPD_PROFILES_RICOH_MH5440_MOCK_H
