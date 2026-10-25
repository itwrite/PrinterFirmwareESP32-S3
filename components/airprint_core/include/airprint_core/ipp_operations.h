// ============================================================================
// ipp_operations.h - IPP operation dispatcher.
//
// Query/job dispatch is enabled when a JobStore is supplied. The no-JobStore
// overload remains query-only and returns standards-defined errors for job
// operations rather than fake success.
// ============================================================================
#ifndef IJPD_AIRPRINT_CORE_IPP_OPERATIONS_H
#define IJPD_AIRPRINT_CORE_IPP_OPERATIONS_H

#include <cstddef>
#include <cstdint>

#include "airprint_core/airprint_capabilities.h"
#include "airprint_core/ipp_job.h"

namespace ijpd {
namespace airprint {

enum class IppDispatchError : uint8_t {
    Ok = 0,
    InvalidArgument,
    OutputTooSmall,
    UnsupportedOperation,
};

const char* ipp_dispatch_error_name(IppDispatchError error) noexcept;

IppDispatchError build_ipp_error_response(
    const IppRequest& request, IppStatus status, const char* unsupported_attribute,
    uint8_t* output, size_t output_capacity, size_t* output_length) noexcept;

IppDispatchError build_ipp_job_response(
    const IppRequest& request, IppStatus status, const IppJobSnapshot* job,
    uint8_t* output, size_t output_capacity, size_t* output_length) noexcept;

IppDispatchError dispatch_ipp_request(
    const IppRequest& request, const AirPrintCapabilities& capabilities,
    uint8_t* output, size_t output_capacity, size_t* output_length) noexcept;

IppDispatchError dispatch_ipp_request_with_jobs(
    const IppRequest& request, const AirPrintCapabilities& capabilities,
    IppJobStore* jobs, uint64_t now_ms,
    uint8_t* output, size_t output_capacity, size_t* output_length) noexcept;

}  // namespace airprint
}  // namespace ijpd

#endif  // IJPD_AIRPRINT_CORE_IPP_OPERATIONS_H
