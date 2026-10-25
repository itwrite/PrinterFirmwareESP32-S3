// ============================================================================
// ipp_print_job.h - Streaming Print-Job ingestion without document decoding.
//
// The stream owns a bounded metadata prefix, creates one IppJobStore job, and
// forwards document bytes to a caller-owned backend sink.
// ============================================================================
#ifndef IJPD_AIRPRINT_CORE_IPP_PRINT_JOB_H
#define IJPD_AIRPRINT_CORE_IPP_PRINT_JOB_H

#include <cstddef>
#include <cstdint>

#include "airprint_core/ipp_job.h"
#include "airprint_core/ipp_request.h"

namespace ijpd {
namespace airprint {

inline constexpr size_t kMaxPrintJobMetadataBytes = 32768;
inline constexpr size_t kPrintJobTextMax = 128;

struct IppPrintBackend {
    void* user = nullptr;
    IppJobError (*begin)(void* user, const IppRequest& request,
                         const IppJobSnapshot& job) = nullptr;
    IppJobError (*write)(void* user, const uint8_t* data, size_t length) = nullptr;
    IppJobError (*end)(void* user, bool success) = nullptr;
    IppJobError (*cancel)(void* user) = nullptr;
    uint32_t (*pages_completed)(void* user) = nullptr;
};

enum class IppPrintStreamState : uint8_t {
    Idle = 0,
    ReadingMetadata,
    ReceivingDocument,
    Completed,
    Canceled,
    Aborted,
    Failed,
};

enum class IppPrintStreamError : uint8_t {
    Ok = 0,
    InvalidArgument,
    InvalidState,
    MetadataTooLarge,
    MalformedRequest,
    UnsupportedOperation,
    UnsupportedDocumentFormat,
    JobAlreadyRunning,
    BackendError,
    BodyTooLarge,
    EmptyDocument,
    IncompleteMetadata,
};

const char* ipp_print_stream_error_name(IppPrintStreamError error) noexcept;

struct IppPrintJobLimits {
    const char* printer_uri = nullptr;
    const char* document_format = "image/urf";
    uint64_t max_document_bytes = 64ull * 1024ull * 1024ull;
    size_t max_metadata_bytes = kMaxPrintJobMetadataBytes;
};

struct IppPrintFeedResult {
    IppPrintStreamError error = IppPrintStreamError::Ok;
    size_t consumed = 0;
    bool metadata_complete = false;
    bool message_complete = false;
};

class IppPrintJobStream {
public:
    IppPrintJobStream(IppJobStore* jobs, const IppPrintBackend& backend,
                      const IppPrintJobLimits& limits = {}) noexcept;

    void configure_limits(const IppPrintJobLimits& limits) noexcept;
    void configure_backend(const IppPrintBackend& backend) noexcept;

    void reset() noexcept;
    IppPrintFeedResult feed(const uint8_t* data, size_t length,
                            uint64_t now_ms) noexcept;
    IppPrintStreamError finish(uint64_t now_ms) noexcept;
    IppPrintStreamError cancel(uint64_t now_ms) noexcept;

    IppPrintStreamState state() const noexcept { return state_; }
    uint64_t document_bytes() const noexcept { return document_bytes_; }
    const IppRequest& request() const noexcept { return request_; }
    const IppJobSnapshot& job() const noexcept { return job_; }

private:
    IppPrintFeedResult fail_(IppPrintStreamError error, size_t consumed) noexcept;
    IppPrintStreamError begin_job_(uint64_t now_ms) noexcept;
    IppPrintStreamError write_document_(const uint8_t* data, size_t length) noexcept;
    IppPrintStreamError copy_attribute_(const char* name, char* output,
                                        size_t capacity, const char* fallback) noexcept;
    IppPrintStreamError map_job_error_(IppJobError error) const noexcept;
    void abort_backend_(uint64_t now_ms, const char* reason) noexcept;

    IppJobStore* jobs_ = nullptr;
    IppPrintBackend backend_{};
    IppPrintJobLimits limits_{};
    IppPrintStreamState state_ = IppPrintStreamState::Idle;
    uint8_t metadata_[kMaxPrintJobMetadataBytes] = {};
    size_t metadata_length_ = 0;
    size_t metadata_limit_ = kMaxPrintJobMetadataBytes;
    IppRequest request_{};
    IppJobSnapshot job_{};
    char printer_uri_[kPrintJobTextMax] = {};
    char job_name_[kPrintJobTextMax] = {};
    char user_name_[kPrintJobTextMax] = {};
    uint64_t document_bytes_ = 0;
};

}  // namespace airprint
}  // namespace ijpd

#endif  // IJPD_AIRPRINT_CORE_IPP_PRINT_JOB_H
