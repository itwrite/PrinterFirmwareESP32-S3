// airprint_sample.cpp - Golden sample manifest validation.
#include "airprint_core/airprint_sample.h"

#include <cstring>

namespace ijpd {
namespace airprint {

const char* airprint_sample_error_name(AirPrintSampleError error) noexcept {
    switch (error) {
        case AirPrintSampleError::Ok: return "ok";
        case AirPrintSampleError::InvalidArgument: return "invalid_argument";
        case AirPrintSampleError::InvalidManifest: return "invalid_manifest";
        case AirPrintSampleError::UnsupportedFormat: return "unsupported_format";
        case AirPrintSampleError::BodyRangeInvalid: return "body_range_invalid";
        case AirPrintSampleError::Sha256Invalid: return "sha256_invalid";
    }
    return "unknown";
}

AirPrintSampleError validate_airprint_sample(
    const AirPrintSampleManifest& manifest) noexcept {
    if (manifest.sample_name[0] == '\0' || manifest.document_format[0] == '\0' ||
        manifest.ipp_body_bytes == 0 || manifest.document_bytes == 0) {
        return AirPrintSampleError::InvalidManifest;
    }
    if (manifest.document_offset > manifest.ipp_body_bytes ||
        manifest.document_bytes > manifest.ipp_body_bytes - manifest.document_offset) {
        return AirPrintSampleError::BodyRangeInvalid;
    }
    if (std::strcmp(manifest.document_format, "image/urf") != 0 &&
        std::strcmp(manifest.document_format, "image/pwg-raster") != 0 &&
        std::strcmp(manifest.document_format, "image/apple-raster") != 0) {
        return AirPrintSampleError::UnsupportedFormat;
    }
    if (std::strlen(manifest.sha256) != 64) return AirPrintSampleError::Sha256Invalid;
    for (size_t i = 0; i < 64; ++i) {
        const char c = manifest.sha256[i];
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                         (c >= 'A' && c <= 'F');
        if (!hex) return AirPrintSampleError::Sha256Invalid;
    }
    if (manifest.expected_values_known &&
        (manifest.expected_width_px == 0 || manifest.expected_height_px == 0 ||
         manifest.expected_dpi_x == 0 || manifest.expected_dpi_y == 0)) {
        return AirPrintSampleError::InvalidManifest;
    }
    return AirPrintSampleError::Ok;
}

}  // namespace airprint
}  // namespace ijpd
