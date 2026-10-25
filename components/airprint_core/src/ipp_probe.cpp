// ipp_probe.cpp - Bounded IPP query/job session.
#include "airprint_core/ipp_probe.h"

#include <cstring>

namespace ijpd {
namespace airprint {

const char* ipp_probe_error_name(IppProbeError error) noexcept {
    switch (error) {
        case IppProbeError::Ok: return "ok";
        case IppProbeError::InvalidArgument: return "invalid_argument";
        case IppProbeError::InvalidCapabilities: return "invalid_capabilities";
        case IppProbeError::InvalidState: return "invalid_state";
        case IppProbeError::RequestTooLarge: return "request_too_large";
        case IppProbeError::OutputTooSmall: return "output_too_small";
    }
    return "unknown";
}

IppProbeError IppProbeSession::configure(const AirPrintCapabilities& capabilities,
                                         const IppProbeLimits& limits) noexcept {
    if (validate_airprint_capabilities(capabilities) != AirPrintResponseError::Ok) {
        configured_ = false;
        return IppProbeError::InvalidCapabilities;
    }
    if (limits.max_request_bytes == 0 ||
        limits.max_request_bytes > kMaxIppProbeRequestBytes) {
        configured_ = false;
        return IppProbeError::InvalidArgument;
    }
    capabilities_ = capabilities;
    limits_ = limits;
    configured_ = true;
    reset();
    return IppProbeError::Ok;
}

void IppProbeSession::reset() noexcept {
    request_length_ = 0;
    request_ = IppRequest{};
    failed_ = false;
    finished_ = false;
    document_overflowed_ = false;
}

IppProbeFeedResult IppProbeSession::feed(const uint8_t* data, size_t length) noexcept {
    if (!configured_ || failed_ || finished_) {
        return {IppProbeError::InvalidState, 0};
    }
    if (data == nullptr && length != 0) {
        return {IppProbeError::InvalidArgument, 0};
    }
    if (length > limits_.max_request_bytes - request_length_) {
        if (limits_.discard_document_overflow) {
            const size_t remaining = limits_.max_request_bytes - request_length_;
            if (remaining != 0) {
                std::memcpy(request_data_ + request_length_, data, remaining);
                request_length_ += remaining;
            }
            document_overflowed_ = true;
            return {IppProbeError::Ok, length};
        }
        failed_ = true;
        return {IppProbeError::RequestTooLarge, 0};
    }
    if (length != 0) {
        std::memcpy(request_data_ + request_length_, data, length);
        request_length_ += length;
    }
    return {IppProbeError::Ok, length};
}

IppProbeError IppProbeSession::finish(uint8_t* output, size_t output_capacity,
                                      size_t* output_length, uint64_t now_ms) noexcept {
    if (output == nullptr || output_length == nullptr) return IppProbeError::InvalidArgument;
    *output_length = 0;
    if (!configured_ || failed_ || finished_) return IppProbeError::InvalidState;

    const IppRequestPolicy policy{capabilities_.document_format, true, false, true};
    (void)parse_ipp_request(request_data_, request_length_, policy, &request_);
    const IppDispatchError dispatch = limits_.jobs != nullptr
                                          ? dispatch_ipp_request_with_jobs(
                                                request_, capabilities_, limits_.jobs, now_ms,
                                                output, output_capacity, output_length)
                                          : dispatch_ipp_request(
                                                request_, capabilities_, output, output_capacity,
                                                output_length);
    if (dispatch == IppDispatchError::InvalidArgument) return IppProbeError::InvalidArgument;
    if (dispatch == IppDispatchError::OutputTooSmall) return IppProbeError::OutputTooSmall;
    finished_ = true;
    return IppProbeError::Ok;
}

}  // namespace airprint
}  // namespace ijpd
