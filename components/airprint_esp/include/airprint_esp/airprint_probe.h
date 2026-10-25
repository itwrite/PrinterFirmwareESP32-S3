// ============================================================================
// airprint_probe.h - Opt-in AirPrint query and Print-Job service.
//
// Opt-in AirPrint HTTP service. Query-only mode remains available when no
// printhead is supplied; the explicit print mode streams image/urf into the
// validated host/core bridge.
// ============================================================================
#ifndef IJPD_AIRPRINT_ESP_AIRPRINT_PROBE_H
#define IJPD_AIRPRINT_ESP_AIRPRINT_PROBE_H

#include "airprint_core/airprint_capabilities.h"
#include "airprint_core/apple_raster_print_backend.h"
#include "airprint_core/ipp_print_job.h"
#include "airprint_core/ipp_probe.h"
#include "airprint_esp/airprint_mdns_service.h"
#include "airprint_esp/ipp_http_server.h"
#include "printer_hal/printhead_hal.h"

namespace ijpd {
namespace airprint {

class AirPrintProbe {
public:
    AirPrintProbe() noexcept;
    Err start(const char* hostname, const char* device_id,
              const AirPrintCapabilities& capabilities,
              IPrintheadHal* printhead = nullptr,
              bool accept_print_jobs = false) noexcept;
    void stop() noexcept;

    bool started() const noexcept { return started_; }
    bool has_active_job() const noexcept;

private:
    struct State;
    static bool on_headers_(void* user, const HttpRequestHead& head) noexcept;
    static bool on_body_(void* user, const uint8_t* data, size_t length,
                         bool message_complete) noexcept;
    static HttpResponse on_complete_(void* user) noexcept;

    bool handle_headers_(const HttpRequestHead& head) noexcept;
    bool handle_body_(const uint8_t* data, size_t length) noexcept;
    HttpResponse handle_complete_() noexcept;
    void handle_abort_(HttpAbortReason reason) noexcept;
    bool feed_print_body_(const uint8_t* data, size_t length) noexcept;
    bool feed_query_body_(const uint8_t* data, size_t length) noexcept;
    HttpResponse print_response_() noexcept;
    static IppStatus stream_error_status_(IppPrintStreamError error) noexcept;
    static RasterDecoderError sink_begin_(void* user,
                                          const RasterPageDescriptor& page) noexcept;
    static RasterDecoderError sink_row_(void* user, const uint8_t* data, size_t length,
                                        uint32_t row_index) noexcept;
    static RasterDecoderError sink_end_(void* user, RasterDecoderError status) noexcept;
    static RasterDecoderError sink_cancel_(void* user) noexcept;
    RasterDecoderError begin_sink_(const RasterPageDescriptor& page) noexcept;
    RasterDecoderError write_sink_(const uint8_t* data, size_t length,
                                   uint32_t row_index) noexcept;
    RasterDecoderError end_sink_(RasterDecoderError status) noexcept;
    RasterDecoderError cancel_sink_() noexcept;
    void finish_display_job_() noexcept;

    IppHttpServer http_;
    AirPrintMdnsService mdns_;
    AirPrintCapabilities capabilities_{};
    IPrintheadHal* printhead_ = nullptr;
    bool accept_print_jobs_ = false;
    State* state_ = nullptr;  // PSRAM-owned large state
    enum class BodyKind : uint8_t { Unknown = 0, Query, PrintJob };
    BodyKind body_kind_ = BodyKind::Unknown;
    uint8_t ipp_prefix_[4] = {};
    size_t ipp_prefix_length_ = 0;
    bool started_ = false;
};

}  // namespace airprint
}  // namespace ijpd

#endif  // IJPD_AIRPRINT_ESP_AIRPRINT_PROBE_H
