// ============================================================================
// ipp_probe.h - Bounded IPP query/job session.
//
// This session connects the HTTP body stream to the platform-independent IPP
// parser and dispatcher. Print-Job is handled by the separate streaming backend
// and the shared JobStore serves subsequent job queries.
// ============================================================================
#ifndef IJPD_AIRPRINT_CORE_IPP_PROBE_H
#define IJPD_AIRPRINT_CORE_IPP_PROBE_H

#include <cstddef>
#include <cstdint>

#include "airprint_core/airprint_capabilities.h"
#include "airprint_core/ipp_operations.h"

namespace ijpd {
namespace airprint {

inline constexpr size_t kMaxIppProbeRequestBytes = 32u * 1024u;
inline constexpr size_t kMaxIppProbeResponseBytes = 16u * 1024u;

struct IppProbeLimits {
    size_t max_request_bytes = kMaxIppProbeRequestBytes;
    bool discard_document_overflow = false;
    IppJobStore* jobs = nullptr;
};

enum class IppProbeError : uint8_t {
    Ok = 0,
    InvalidArgument,
    InvalidCapabilities,
    InvalidState,
    RequestTooLarge,
    OutputTooSmall,
};

const char* ipp_probe_error_name(IppProbeError error) noexcept;

struct IppProbeFeedResult {
    IppProbeError error = IppProbeError::Ok;
    size_t consumed = 0;
};

class IppProbeSession {
public:
    IppProbeSession() noexcept = default;

    IppProbeError configure(const AirPrintCapabilities& capabilities,
                            const IppProbeLimits& limits = {}) noexcept;
    void reset() noexcept;

    IppProbeFeedResult feed(const uint8_t* data, size_t length) noexcept;
    IppProbeError finish(uint8_t* output, size_t output_capacity,
                         size_t* output_length, uint64_t now_ms = 0) noexcept;

    bool configured() const noexcept { return configured_; }
    size_t request_bytes() const noexcept { return request_length_; }
    const IppRequest& request() const noexcept { return request_; }

private:
    AirPrintCapabilities capabilities_{};
    IppProbeLimits limits_{};
    uint8_t request_data_[kMaxIppProbeRequestBytes] = {};
    size_t request_length_ = 0;
    IppRequest request_{};
    bool configured_ = false;
    bool failed_ = false;
    bool finished_ = false;
    bool document_overflowed_ = false;
};

}  // namespace airprint
}  // namespace ijpd

#endif  // IJPD_AIRPRINT_CORE_IPP_PROBE_H
