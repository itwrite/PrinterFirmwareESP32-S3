// ============================================================================
// apple_raster_print_backend.h - Opt-in Print-Job to Apple Raster bridge.
//
// This adapter connects IppPrintJobStream document chunks to the verified
// AppleRasterDecoder subset and a caller-owned RasterSink. It is platform
// independent and does not own or invoke a printer HAL.
// ============================================================================
#ifndef IJPD_AIRPRINT_CORE_APPLE_RASTER_PRINT_BACKEND_H
#define IJPD_AIRPRINT_CORE_APPLE_RASTER_PRINT_BACKEND_H

#include "airprint_core/ipp_print_job.h"
#include "airprint_core/raster_decoder.h"

namespace ijpd {
namespace airprint {

class AppleRasterPrintBackend {
public:
    AppleRasterPrintBackend(const RasterDecoderLimits& limits,
                            const RasterSink& sink) noexcept;

    void configure(const RasterDecoderLimits& limits, const RasterSink& sink) noexcept;

    IppPrintBackend backend() noexcept;
    RasterDecoderError decoder_error() const noexcept { return decoder_error_; }
    uint64_t decoded_document_bytes() const noexcept {
        return decoder_.decoded_bytes();
    }
    uint64_t document_bytes() const noexcept {
        return decoder_.document_bytes();
    }

private:
    static IppJobError begin_(void* user, const IppRequest& request,
                              const IppJobSnapshot& job) noexcept;
    static IppJobError write_(void* user, const uint8_t* data, size_t length) noexcept;
    static IppJobError end_(void* user, bool success) noexcept;
    static IppJobError cancel_(void* user) noexcept;
    static uint32_t pages_completed_(void* user) noexcept;

    IppJobError begin_impl_(const IppRequest& request,
                            const IppJobSnapshot& job) noexcept;
    IppJobError write_impl_(const uint8_t* data, size_t length) noexcept;
    IppJobError end_impl_(bool success) noexcept;
    IppJobError cancel_impl_() noexcept;
    uint32_t pages_completed_impl_() const noexcept;

    RasterDecoderLimits limits_{};
    RasterSink sink_{};
    AppleRasterDecoder decoder_{};
    RasterDecoderError decoder_error_ = RasterDecoderError::Ok;
    bool begun_ = false;
};

}  // namespace airprint
}  // namespace ijpd

#endif  // IJPD_AIRPRINT_CORE_APPLE_RASTER_PRINT_BACKEND_H
