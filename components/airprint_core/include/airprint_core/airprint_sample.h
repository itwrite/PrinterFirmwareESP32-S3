// ============================================================================
// airprint_sample.h - Golden sample manifest contract.
// ============================================================================
#ifndef IJPD_AIRPRINT_CORE_AIRPRINT_SAMPLE_H
#define IJPD_AIRPRINT_CORE_AIRPRINT_SAMPLE_H

#include <cstddef>
#include <cstdint>

namespace ijpd {
namespace airprint {

inline constexpr size_t kAirPrintSampleFormatMax = 64;
inline constexpr size_t kAirPrintSampleSha256HexLength = 65;

struct AirPrintSampleManifest {
    char sample_name[128] = {};
    char document_format[kAirPrintSampleFormatMax] = {};
    char sha256[kAirPrintSampleSha256HexLength] = {};
    uint64_t ipp_body_bytes = 0;
    uint64_t document_offset = 0;
    uint64_t document_bytes = 0;
    uint32_t expected_width_px = 0;
    uint32_t expected_height_px = 0;
    uint16_t expected_dpi_x = 0;
    uint16_t expected_dpi_y = 0;
    uint32_t expected_output_crc32 = 0;
    bool expected_values_known = false;
};

enum class AirPrintSampleError : uint8_t {
    Ok = 0,
    InvalidArgument,
    InvalidManifest,
    UnsupportedFormat,
    BodyRangeInvalid,
    Sha256Invalid,
};

const char* airprint_sample_error_name(AirPrintSampleError error) noexcept;
AirPrintSampleError validate_airprint_sample(
    const AirPrintSampleManifest& manifest) noexcept;

}  // namespace airprint
}  // namespace ijpd

#endif  // IJPD_AIRPRINT_CORE_AIRPRINT_SAMPLE_H
