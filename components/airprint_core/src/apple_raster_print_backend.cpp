// apple_raster_print_backend.cpp - Opt-in Print-Job to Apple Raster bridge.
#include "airprint_core/apple_raster_print_backend.h"

namespace ijpd {
namespace airprint {

namespace {

IppJobError map_decoder_error(RasterDecoderError error) noexcept {
    return error == RasterDecoderError::Canceled
               ? IppJobError::InvalidState
               : error == RasterDecoderError::InvalidArgument
                     ? IppJobError::InvalidArgument
                     : error == RasterDecoderError::Ok ||
                               error == RasterDecoderError::NeedMoreData
                           ? IppJobError::Ok
                           : IppJobError::InvalidState;
}

}  // namespace

AppleRasterPrintBackend::AppleRasterPrintBackend(const RasterDecoderLimits& limits,
                                                 const RasterSink& sink) noexcept
    : limits_(limits), sink_(sink) {}

void AppleRasterPrintBackend::configure(const RasterDecoderLimits& limits,
                                        const RasterSink& sink) noexcept {
    if (begun_) return;
    limits_ = limits;
    sink_ = sink;
    decoder_error_ = RasterDecoderError::Ok;
}

IppPrintBackend AppleRasterPrintBackend::backend() noexcept {
    return IppPrintBackend{this, begin_, write_, end_, cancel_, pages_completed_};
}

IppJobError AppleRasterPrintBackend::begin_(void* user, const IppRequest& request,
                                            const IppJobSnapshot& job) noexcept {
    return user == nullptr
               ? IppJobError::InvalidArgument
               : static_cast<AppleRasterPrintBackend*>(user)->begin_impl_(request, job);
}

IppJobError AppleRasterPrintBackend::write_(void* user, const uint8_t* data,
                                            size_t length) noexcept {
    return user == nullptr
               ? IppJobError::InvalidArgument
               : static_cast<AppleRasterPrintBackend*>(user)->write_impl_(data, length);
}

IppJobError AppleRasterPrintBackend::end_(void* user, bool success) noexcept {
    return user == nullptr
               ? IppJobError::InvalidArgument
               : static_cast<AppleRasterPrintBackend*>(user)->end_impl_(success);
}

IppJobError AppleRasterPrintBackend::cancel_(void* user) noexcept {
    return user == nullptr
               ? IppJobError::InvalidArgument
               : static_cast<AppleRasterPrintBackend*>(user)->cancel_impl_();
}

uint32_t AppleRasterPrintBackend::pages_completed_(void* user) noexcept {
    return user == nullptr
               ? 0
               : static_cast<AppleRasterPrintBackend*>(user)->pages_completed_impl_();
}

IppJobError AppleRasterPrintBackend::begin_impl_(const IppRequest& request,
                                                 const IppJobSnapshot&) noexcept {
    if (begun_) return IppJobError::AlreadyActive;
    decoder_error_ = decoder_.begin(limits_, sink_);
    if (decoder_error_ != RasterDecoderError::Ok) return map_decoder_error(decoder_error_);
    begun_ = true;
    (void)request;
    return IppJobError::Ok;
}

IppJobError AppleRasterPrintBackend::write_impl_(const uint8_t* data,
                                                 size_t length) noexcept {
    if (!begun_) return IppJobError::InvalidState;
    size_t consumed = 0;
    decoder_error_ = decoder_.feed(data, length, &consumed);
    if (decoder_error_ != RasterDecoderError::Ok &&
        decoder_error_ != RasterDecoderError::NeedMoreData) {
        return map_decoder_error(decoder_error_);
    }
    if (consumed != length) {
        decoder_error_ = RasterDecoderError::InvalidState;
        return IppJobError::InvalidState;
    }
    return IppJobError::Ok;
}

IppJobError AppleRasterPrintBackend::end_impl_(bool success) noexcept {
    if (!begun_) return IppJobError::InvalidState;
    if (!success) return cancel_impl_();
    decoder_error_ = decoder_.finish();
    begun_ = false;
    return map_decoder_error(decoder_error_);
}

IppJobError AppleRasterPrintBackend::cancel_impl_() noexcept {
    if (!begun_) return IppJobError::Ok;
    if (decoder_error_ != RasterDecoderError::Ok &&
        decoder_error_ != RasterDecoderError::NeedMoreData) {
        begun_ = false;
        return map_decoder_error(decoder_error_);
    }
    decoder_error_ = decoder_.cancel();
    begun_ = false;
    return decoder_error_ == RasterDecoderError::Canceled
               ? IppJobError::Ok
               : IppJobError::InvalidState;
}

uint32_t AppleRasterPrintBackend::pages_completed_impl_() const noexcept {
    return decoder_.pages_completed();
}

}  // namespace airprint
}  // namespace ijpd
