// airprint_probe.cpp - Opt-in AirPrint query and Print-Job service.
#include "airprint_esp/airprint_probe.h"

#include "airprint_core/airprint_mdns.h"
#include "airprint_core/ipp_operations.h"

#include <cstring>
#include <new>

#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "platform_esp/display_service.h"

namespace ijpd {
namespace airprint {
namespace {
constexpr const char* kTag = "airprint";
constexpr uint32_t kMockWidthPx = 2480;
constexpr uint32_t kMockHeightPx = 3508;
constexpr uint32_t kMockRowBytes = kMockWidthPx * 4;
constexpr uint32_t kMockWidthUm = 210000;
constexpr uint32_t kMockDpi = 300;

const char* abort_reason_name(HttpAbortReason reason) noexcept {
    switch (reason) {
        case HttpAbortReason::PeerClosed: return "peer_closed";
        case HttpAbortReason::IdleTimeout: return "idle_timeout";
        case HttpAbortReason::ParserError: return "parser_error";
        case HttpAbortReason::CallbackRejected: return "callback_rejected";
        case HttpAbortReason::ServerStopping: return "server_stopping";
        case HttpAbortReason::SocketError: return "socket_error";
    }
    return "unknown";
}
}

struct AirPrintProbe::State {
    AirPrintMdnsRecord mdns_record{};
    IppProbeSession session;
    IppJobStore jobs;
    RasterSink sink{};
    IppPrintBackend print_backend{};
    AppleRasterPrintBackend raster_backend;
    IppPrintJobLimits print_limits{};
    IppPrintJobStream print_job;
    bool page_started = false;
    bool display_job_started = false;
    RasterPageDescriptor page{};
    uint8_t cmyk_row[kMockRowBytes] = {};
    uint8_t response[kMaxIppProbeResponseBytes] = {};
    size_t response_length = 0;

    State() noexcept
        : raster_backend({}, sink), print_job(&jobs, print_backend, print_limits) {}
};

AirPrintProbe::AirPrintProbe() noexcept
    = default;

bool AirPrintProbe::has_active_job() const noexcept {
    if (state_ == nullptr) return false;
    const IppPrintStreamState stream = state_->print_job.state();
    return stream == IppPrintStreamState::ReadingMetadata ||
           stream == IppPrintStreamState::ReceivingDocument;
}

Err AirPrintProbe::start(const char* hostname, const char* device_id,
                         const AirPrintCapabilities& capabilities,
                         IPrintheadHal* printhead,
                         bool accept_print_jobs) noexcept {
    if (started_ || hostname == nullptr || device_id == nullptr) {
        return Err::INVALID_ARGUMENT;
    }
    if (validate_airprint_capabilities(capabilities) != AirPrintResponseError::Ok) {
        return Err::INVALID_ARGUMENT;
    }
    if (capabilities.printer_uri == nullptr || capabilities.printer_uri[0] == '\0') {
        return Err::INVALID_ARGUMENT;
    }

    capabilities_ = capabilities;
    if (state_ == nullptr) {
        state_ = static_cast<State*>(heap_caps_malloc(sizeof(State), MALLOC_CAP_SPIRAM));
        if (state_ == nullptr) return Err::OUT_OF_MEMORY;
        new (state_) State();
    }
    State& state = *state_;
    if (build_airprint_mdns_record(capabilities_, device_id, hostname, &state.mdns_record) !=
        AirPrintMdnsError::Ok) {
        return Err::INVALID_ARGUMENT;
    }
    state.sink = RasterSink{this, sink_begin_, sink_row_, sink_end_, sink_cancel_};
    state.raster_backend.configure({}, state.sink);
    state.print_backend = state.raster_backend.backend();
    state.print_job.configure_backend(state.print_backend);
    printhead_ = printhead;
    accept_print_jobs_ = accept_print_jobs && printhead_ != nullptr;
    capabilities_.accepting_jobs = accept_print_jobs_;
    IppProbeLimits limits;
    limits.discard_document_overflow = true;
    limits.jobs = &state.jobs;
    const IppProbeError configured = state.session.configure(capabilities_, limits);
    if (configured != IppProbeError::Ok) return Err::INVALID_ARGUMENT;

    state.print_limits.printer_uri = capabilities_.printer_uri;
    state.print_limits.document_format = capabilities_.document_format;
    state.print_limits.max_document_bytes = 16ull * 1024ull * 1024ull;
    state.print_limits.max_metadata_bytes = kMaxPrintJobMetadataBytes;
    state.print_job.configure_limits(state.print_limits);

    IppHttpServer::Host host;
    host.user = this;
    host.on_headers = on_headers_;
    host.on_body = on_body_;
    host.on_complete = on_complete_;
    host.on_abort = [](void* user, HttpAbortReason reason) {
        if (user != nullptr) static_cast<AirPrintProbe*>(user)->handle_abort_(reason);
    };
    HttpParserLimits http_limits;
    http_limits.max_body_bytes = state_->print_limits.max_document_bytes +
                                 state_->print_limits.max_metadata_bytes;
    const Err http_error = http_.start(capabilities.ipp_port, host, http_limits);
    if (http_error != Err::OK) return http_error;

    const Err mdns_error = mdns_.start(hostname, device_id, state.mdns_record);
    if (mdns_error != Err::OK) {
        http_.stop();
        return mdns_error;
    }
    started_ = true;
    return Err::OK;
}

void AirPrintProbe::stop() noexcept {
    mdns_.stop();
    http_.stop();
    started_ = false;
    if (state_ != nullptr) {
        state_->session.reset();
        if (state_->print_job.state() == IppPrintStreamState::ReadingMetadata ||
            state_->print_job.state() == IppPrintStreamState::ReceivingDocument) {
            (void)state_->print_job.cancel(0);
        }
        finish_display_job_();
    }
}

bool AirPrintProbe::on_headers_(void* user, const HttpRequestHead& head) noexcept {
    return user != nullptr && static_cast<AirPrintProbe*>(user)->handle_headers_(head);
}

bool AirPrintProbe::on_body_(void* user, const uint8_t* data, size_t length,
                             bool message_complete) noexcept {
    (void)message_complete;
    return user != nullptr && static_cast<AirPrintProbe*>(user)->handle_body_(data, length);
}

HttpResponse AirPrintProbe::on_complete_(void* user) noexcept {
    if (user == nullptr) return {};
    return static_cast<AirPrintProbe*>(user)->handle_complete_();
}

bool AirPrintProbe::handle_headers_(const HttpRequestHead& head) noexcept {
    ESP_LOGI(kTag, "HTTP request: content_type=%.*s length=%llu chunked=%s expect=%s",
             static_cast<int>(head.content_type.length), head.content_type.data,
             static_cast<unsigned long long>(head.content_length),
             head.chunked ? "yes" : "no", head.expect_continue ? "yes" : "no");
    if (state_ != nullptr) {
        // A new request must not inherit a preview from an interrupted one.
        finish_display_job_();
        state_->session.reset();
        state_->response_length = 0;
    }
    body_kind_ = BodyKind::Unknown;
    ipp_prefix_length_ = 0;
    if (state_ != nullptr) state_->page_started = false;
    if (accept_print_jobs_ && state_ != nullptr) state_->print_job.reset();
    return true;
}

bool AirPrintProbe::handle_body_(const uint8_t* data, size_t length) noexcept {
    if (body_kind_ == BodyKind::PrintJob) return feed_print_body_(data, length);
    if (body_kind_ == BodyKind::Query) return feed_query_body_(data, length);

    size_t offset = 0;
    while (offset < length && ipp_prefix_length_ < sizeof(ipp_prefix_)) {
        ipp_prefix_[ipp_prefix_length_++] = data[offset++];
    }
    if (ipp_prefix_length_ < sizeof(ipp_prefix_)) return true;
    const uint16_t operation = static_cast<uint16_t>(ipp_prefix_[2] << 8) | ipp_prefix_[3];
    ESP_LOGI(kTag, "IPP operation=0x%04x", static_cast<unsigned>(operation));
    body_kind_ = operation == static_cast<uint16_t>(IppOperation::PrintJob) &&
                         accept_print_jobs_
                     ? BodyKind::PrintJob
                     : BodyKind::Query;
    if (body_kind_ == BodyKind::PrintJob) {
        if (!feed_print_body_(ipp_prefix_, sizeof(ipp_prefix_))) {
            ESP_LOGE(kTag, "Print-Job prefix rejected: stream=%u decoder=%s",
                     static_cast<unsigned>(state_->print_job.state()),
                     raster_decoder_error_name(state_->raster_backend.decoder_error()));
            return false;
        }
    } else {
        if (state_ == nullptr ||
            state_->session.feed(ipp_prefix_, sizeof(ipp_prefix_)).error != IppProbeError::Ok) {
            ESP_LOGE(kTag, "IPP query prefix rejected");
            return false;
        }
    }
    if (offset < length) {
        const bool accepted = body_kind_ == BodyKind::PrintJob
                                  ? feed_print_body_(data + offset, length - offset)
                                  : feed_query_body_(data + offset, length - offset);
        if (!accepted) {
            ESP_LOGE(kTag, "IPP body rejected: kind=%u stream=%u decoder=%s",
                     static_cast<unsigned>(body_kind_),
                     state_ != nullptr ? static_cast<unsigned>(state_->print_job.state()) : 0u,
                     state_ != nullptr
                         ? raster_decoder_error_name(state_->raster_backend.decoder_error())
                         : "no_state");
        }
        return accepted;
    }
    return true;
}

HttpResponse AirPrintProbe::handle_complete_() noexcept {
    if (body_kind_ == BodyKind::PrintJob) return print_response_();
    HttpResponse response;
    response.status_code = 200;
    response.reason = "OK";
    response.content_type = "application/ipp";
    if (state_ == nullptr) return response;
    state_->response_length = 0;
    if (state_->session.finish(state_->response, sizeof(state_->response),
                               &state_->response_length,
                               static_cast<uint64_t>(esp_timer_get_time() / 1000)) !=
        IppProbeError::Ok) {
        static constexpr char kError[] = "ipp probe failed";
        response.status_code = 500;
        response.reason = "Internal Server Error";
        response.content_type = "text/plain; charset=utf-8";
        response.body = reinterpret_cast<const uint8_t*>(kError);
        response.body_length = sizeof(kError) - 1;
        return response;
    }
    response.body = state_->response;
    response.body_length = state_->response_length;
    return response;
}

bool AirPrintProbe::feed_query_body_(const uint8_t* data, size_t length) noexcept {
    return state_ != nullptr && state_->session.feed(data, length).error == IppProbeError::Ok;
}

bool AirPrintProbe::feed_print_body_(const uint8_t* data, size_t length) noexcept {
    if (state_ == nullptr || (data == nullptr && length != 0)) return false;
    size_t offset = 0;
    while (offset < length) {
        const IppPrintFeedResult result = state_->print_job.feed(
            data + offset, length - offset,
            static_cast<uint64_t>(esp_timer_get_time() / 1000));
        if (result.error != IppPrintStreamError::Ok) {
            const IppRequest& request = state_->print_job.request();
            ESP_LOGE(kTag, "Print-Job stream error=%s consumed=%u state=%u doc=%llu "
                     "ipp_error=%s ipp_status=0x%04x attrs=%u error_attr_len=%u decoder=%s",
                     ipp_print_stream_error_name(result.error),
                     static_cast<unsigned>(result.consumed),
                     static_cast<unsigned>(state_->print_job.state()),
                     static_cast<unsigned long long>(state_->print_job.document_bytes()),
                     ipp_request_error_name(request.error),
                     static_cast<unsigned>(request.status),
                     static_cast<unsigned>(request.attribute_count),
                     static_cast<unsigned>(request.error_attribute_length),
                     raster_decoder_error_name(state_->raster_backend.decoder_error()));
            return false;
        }
        if (result.consumed == 0) return false;
        offset += result.consumed;
    }
    return true;
}

void AirPrintProbe::handle_abort_(HttpAbortReason reason) noexcept {
    ESP_LOGW(kTag, "HTTP request aborted: reason=%s stream=%u job=%lu doc=%llu decoder=%s",
             abort_reason_name(reason),
             state_ != nullptr ? static_cast<unsigned>(state_->print_job.state()) : 0u,
             state_ != nullptr ? static_cast<unsigned long>(state_->print_job.job().id) : 0ul,
             state_ != nullptr
                 ? static_cast<unsigned long long>(state_->print_job.document_bytes())
                 : 0ull,
             state_ != nullptr
                 ? raster_decoder_error_name(state_->raster_backend.decoder_error())
                 : "no_state");
    if (state_ != nullptr &&
        (state_->print_job.state() == IppPrintStreamState::ReadingMetadata ||
         state_->print_job.state() == IppPrintStreamState::ReceivingDocument)) {
        (void)state_->print_job.cancel(static_cast<uint64_t>(esp_timer_get_time() / 1000));
    }
    finish_display_job_();
}

HttpResponse AirPrintProbe::print_response_() noexcept {
    HttpResponse response;
    response.status_code = 200;
    response.reason = "OK";
    response.content_type = "application/ipp";
    if (state_ == nullptr) return response;
    state_->response_length = 0;
    const IppPrintStreamError result =
        state_->print_job.finish(static_cast<uint64_t>(esp_timer_get_time() / 1000));
    ESP_LOGI(kTag, "Print-Job finished: result=%s bytes=%llu",
             ipp_print_stream_error_name(result),
             static_cast<unsigned long long>(state_->print_job.document_bytes()));
    IppStatus status = result == IppPrintStreamError::Ok
                           ? IppStatus::SuccessfulOk
                           : stream_error_status_(result);
    IppJobSnapshot snapshot{};
    const IppJobSnapshot* job = nullptr;
    if (state_->print_job.job().id != 0) {
        snapshot = state_->print_job.job();
        job = &snapshot;
    }
    if (build_ipp_job_response(state_->print_job.request(), status, job, state_->response,
                               sizeof(state_->response), &state_->response_length) !=
        IppDispatchError::Ok) {
        response.status_code = 500;
        response.reason = "Internal Server Error";
        response.content_type = "text/plain; charset=utf-8";
        static constexpr char kError[] = "ipp print response failed";
        response.body = reinterpret_cast<const uint8_t*>(kError);
        response.body_length = sizeof(kError) - 1;
        return response;
    }
    response.body = state_->response;
    response.body_length = state_->response_length;
    return response;
}

IppStatus AirPrintProbe::stream_error_status_(IppPrintStreamError error) noexcept {
    switch (error) {
        case IppPrintStreamError::UnsupportedDocumentFormat:
            return IppStatus::ClientErrorDocumentFormatNotSupported;
        case IppPrintStreamError::BodyTooLarge:
            return IppStatus::ClientErrorRequestEntityTooLarge;
        case IppPrintStreamError::EmptyDocument:
        case IppPrintStreamError::MalformedRequest:
        case IppPrintStreamError::IncompleteMetadata:
            return IppStatus::ClientErrorDocumentFormatError;
        case IppPrintStreamError::BackendError:
            return IppStatus::ServerErrorDeviceError;
        default:
            return IppStatus::ClientErrorBadRequest;
    }
}

RasterDecoderError AirPrintProbe::sink_begin_(void* user,
                                              const RasterPageDescriptor& page) noexcept {
    return user == nullptr
               ? RasterDecoderError::InvalidArgument
               : static_cast<AirPrintProbe*>(user)->begin_sink_(page);
}

RasterDecoderError AirPrintProbe::sink_row_(void* user, const uint8_t* data, size_t length,
                                            uint32_t row_index) noexcept {
    return user == nullptr
               ? RasterDecoderError::InvalidArgument
               : static_cast<AirPrintProbe*>(user)->write_sink_(data, length, row_index);
}

RasterDecoderError AirPrintProbe::sink_end_(void* user, RasterDecoderError status) noexcept {
    return user == nullptr
               ? RasterDecoderError::InvalidArgument
               : static_cast<AirPrintProbe*>(user)->end_sink_(status);
}

RasterDecoderError AirPrintProbe::sink_cancel_(void* user) noexcept {
    return user == nullptr
               ? RasterDecoderError::InvalidArgument
               : static_cast<AirPrintProbe*>(user)->cancel_sink_();
}

RasterDecoderError AirPrintProbe::begin_sink_(const RasterPageDescriptor& page) noexcept {
    if (state_ == nullptr || printhead_ == nullptr || page.width_px == 0 || page.bytes_per_row == 0) {
        return RasterDecoderError::BackendError;
    }
    const uint32_t bytes_per_pixel =
        page.pixel_format == RasterPixelFormat::CMYK8888
            ? 4u
            : page.pixel_format == RasterPixelFormat::RGB888 ? 3u : 1u;
    if (page.width_px == 0 || page.bytes_per_row / page.width_px != bytes_per_pixel) {
        return RasterDecoderError::UnsupportedFormat;
    }
    state_->page = page;
    state_->page_started = false;
    state_->display_job_started = DisplayService::instance().begin_job(
        kMockWidthPx, kMockHeightPx, 8u, 4u, kMockRowBytes,
        nullptr, 0u, true, 4u, kMockWidthUm, kMockDpi) == Err::OK;
    if (printhead_->begin(PrintheadConfig{kMockRowBytes, 200u, 0}) != Err::OK) {
        (void)printhead_->stop();
        finish_display_job_();
        return RasterDecoderError::BackendError;
    }
    state_->page_started = true;
    return RasterDecoderError::Ok;
}

RasterDecoderError AirPrintProbe::write_sink_(const uint8_t* data, size_t length,
                                              uint32_t row_index) noexcept {
    if (state_ == nullptr || !state_->page_started || data == nullptr ||
        length != state_->page.bytes_per_row ||
        printhead_ == nullptr) {
        return RasterDecoderError::BackendError;
    }
    for (uint32_t pixel = 0; pixel < kMockWidthPx; ++pixel) {
        const uint32_t source = static_cast<uint32_t>(
            (static_cast<uint64_t>(pixel) * state_->page.width_px) / kMockWidthPx);
        uint8_t gray = 0;
        if (state_->page.pixel_format == RasterPixelFormat::RGB888) {
            const size_t source_offset = static_cast<size_t>(source) * 3u;
            const size_t safe_offset = source_offset + 2u < length ? source_offset :
                                       length >= 3u ? length - 3u : 0u;
            const uint8_t red = data[safe_offset];
            const uint8_t green = data[safe_offset + 1u];
            const uint8_t blue = data[safe_offset + 2u];
            gray = static_cast<uint8_t>((77u * red + 150u * green + 29u * blue) >> 8);
        } else if (state_->page.pixel_format == RasterPixelFormat::CMYK8888) {
            const size_t source_offset = static_cast<size_t>(source) * 4u;
            const size_t safe_offset = source_offset + 3u < length ? source_offset :
                                       length >= 4u ? length - 4u : 0u;
            const uint8_t cyan = data[safe_offset];
            const uint8_t magenta = data[safe_offset + 1u];
            const uint8_t yellow = data[safe_offset + 2u];
            const uint8_t black = data[safe_offset + 3u];
            const uint8_t red = static_cast<uint8_t>(255u -
                (cyan > 255u - black ? 255u : cyan + black));
            const uint8_t green = static_cast<uint8_t>(255u -
                (magenta > 255u - black ? 255u : magenta + black));
            const uint8_t blue = static_cast<uint8_t>(255u -
                (yellow > 255u - black ? 255u : yellow + black));
            gray = static_cast<uint8_t>((77u * red + 150u * green + 29u * blue) >> 8);
        } else {
            gray = data[source < length ? source : length - 1];
        }
        state_->cmyk_row[pixel * 4 + 0] = 0;
        state_->cmyk_row[pixel * 4 + 1] = 0;
        state_->cmyk_row[pixel * 4 + 2] = 0;
        state_->cmyk_row[pixel * 4 + 3] = static_cast<uint8_t>(255u - gray);
    }
    // AirPrint and IJPD now share the same visible-row boundary: the preview
    // is updated immediately before the corresponding HAL submission.
    const uint32_t output_begin = static_cast<uint32_t>(
        (static_cast<uint64_t>(row_index) * kMockHeightPx) / state_->page.height_px);
    const uint32_t output_end = static_cast<uint32_t>(
        (static_cast<uint64_t>(row_index + 1u) * kMockHeightPx) / state_->page.height_px);
    for (uint32_t output_row = output_begin; output_row < output_end; ++output_row) {
        DisplayService::instance().feed_row(output_row, state_->cmyk_row,
                                            sizeof(state_->cmyk_row));
        if (printhead_->submitRaster(state_->cmyk_row, sizeof(state_->cmyk_row),
                                     RasterMeta{output_row}) != Err::OK) {
            return RasterDecoderError::BackendError;
        }
    }
    return RasterDecoderError::Ok;
}

RasterDecoderError AirPrintProbe::end_sink_(RasterDecoderError status) noexcept {
    if (state_ != nullptr) state_->page_started = false;
    RasterDecoderError result = status;
    if (printhead_ == nullptr) {
        finish_display_job_();
        return result;
    }
    if (status != RasterDecoderError::Ok) {
        if (printhead_->stop() != Err::OK) result = RasterDecoderError::BackendError;
        finish_display_job_();
        return result;
    }
    if (printhead_->waitUntilIdle(5000) != Err::OK) {
        result = RasterDecoderError::BackendError;
        (void)printhead_->stop();
    }
    finish_display_job_();
    return result;
}

RasterDecoderError AirPrintProbe::cancel_sink_() noexcept {
    if (state_ != nullptr) state_->page_started = false;
    const Err stop_error = printhead_ == nullptr ? Err::OK : printhead_->stop();
    finish_display_job_();
    return stop_error == Err::OK ? RasterDecoderError::Ok
                                 : RasterDecoderError::BackendError;
}

void AirPrintProbe::finish_display_job_() noexcept {
    if (state_ == nullptr || !state_->display_job_started) return;
    DisplayService::instance().end_job();
    state_->display_job_started = false;
}

}  // namespace airprint
}  // namespace ijpd
