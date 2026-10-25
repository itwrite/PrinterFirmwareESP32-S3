// ============================================================================
// airprint_capabilities.h - Truthful capability registry and IPP response builder.
//
// This is a platform-independent first slice. It emits only attributes backed by
// explicit capability fields; it does not invent collections or media geometry.
// ============================================================================
#ifndef IJPD_AIRPRINT_CORE_AIRPRINT_CAPABILITIES_H
#define IJPD_AIRPRINT_CORE_AIRPRINT_CAPABILITIES_H

#include <cstddef>
#include <cstdint>

#include "airprint_core/ipp_request.h"

namespace ijpd {
namespace airprint {

struct AirPrintCapabilities {
    bool enabled = false;
    uint16_t ipp_port = 631;
    const char* printer_uri = nullptr;
    const char* printer_more_info = nullptr;
    const char* printer_name = nullptr;
    const char* make_and_model = nullptr;
    const char* printer_info = nullptr;
    const char* printer_location = "";
    const char* media_keyword = nullptr;
    const char* document_format = "image/urf";
    const char* urf_supported = nullptr;
    uint16_t dpi_x = 0;
    uint16_t dpi_y = 0;
    bool color_supported = false;
    bool duplex_supported = false;
    bool accepting_jobs = true;
    uint32_t queued_job_count = 0;
};

enum class AirPrintResponseError : uint8_t {
    Ok = 0,
    InvalidArgument,
    InvalidCapabilities,
    UnsupportedOperation,
    OutputTooSmall,
};

const char* airprint_response_error_name(AirPrintResponseError error) noexcept;

AirPrintResponseError validate_airprint_capabilities(
    const AirPrintCapabilities& capabilities) noexcept;

AirPrintResponseError build_get_printer_attributes_response(
    const IppRequest& request, const AirPrintCapabilities& capabilities,
    uint8_t* output, size_t output_capacity, size_t* output_length) noexcept;

}  // namespace airprint
}  // namespace ijpd

#endif  // IJPD_AIRPRINT_CORE_AIRPRINT_CAPABILITIES_H
