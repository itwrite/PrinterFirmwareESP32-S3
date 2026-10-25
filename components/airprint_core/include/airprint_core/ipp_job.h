// ============================================================================
// ipp_job.h - Single-active bounded IPP Job lifecycle.
// ============================================================================
#ifndef IJPD_AIRPRINT_CORE_IPP_JOB_H
#define IJPD_AIRPRINT_CORE_IPP_JOB_H

#include <cstddef>
#include <cstdint>

#include "airprint_core/ipp_types.h"

namespace ijpd {
namespace airprint {

inline constexpr size_t kIppJobIdTextMax = 64;
inline constexpr size_t kIppJobUriTextMax = 128;
inline constexpr size_t kIppJobNameTextMax = 128;
inline constexpr size_t kIppJobUserTextMax = 128;
inline constexpr size_t kIppJobReasonTextMax = 64;
inline constexpr size_t kIppJobFormatTextMax = 64;

enum class IppJobState : uint8_t {
    Idle = 0,
    Pending,
    Processing,
    Completed,
    Canceled,
    Aborted,
};

const char* ipp_job_state_name(IppJobState state) noexcept;
int32_t ipp_job_state_value(IppJobState state) noexcept;

enum class IppJobError : uint8_t {
    Ok = 0,
    InvalidArgument,
    AlreadyActive,
    NoActiveJob,
    InvalidState,
    IdMismatch,
    OutputTooSmall,
};

const char* ipp_job_error_name(IppJobError error) noexcept;

struct IppJobSnapshot {
    IppJobState state = IppJobState::Idle;
    uint32_t id = 0;
    char printer_uri[kIppJobUriTextMax] = {};
    char job_id[kIppJobIdTextMax] = {};
    char job_uri[kIppJobUriTextMax] = {};
    char job_name[kIppJobNameTextMax] = {};
    char user_name[kIppJobUserTextMax] = {};
    char state_reason[kIppJobReasonTextMax] = "none";
    char document_format[kIppJobFormatTextMax] = {};
    uint32_t pages_completed = 0;
    uint32_t job_k_octets = 0;
    uint64_t created_ms = 0;
    uint64_t processing_ms = 0;
    uint64_t completed_ms = 0;
};

class IppJobStore {
public:
    IppJobStore() noexcept = default;

    IppJobError create(const char* printer_uri, const char* job_name, const char* user_name,
                       const char* document_format, uint64_t now_ms,
                       IppJobSnapshot* out) noexcept;
    IppJobError start_processing(uint32_t id, uint64_t now_ms) noexcept;
    IppJobError complete(uint32_t id, uint64_t now_ms,
                         uint32_t pages_completed, uint32_t job_k_octets) noexcept;
    IppJobError abort(uint32_t id, const char* reason, uint64_t now_ms) noexcept;
    IppJobError cancel(uint32_t id, uint64_t now_ms) noexcept;

    IppJobError get(uint32_t id, IppJobSnapshot* out) const noexcept;
    IppJobError get_active(IppJobSnapshot* out) const noexcept;
    IppJobError get_last(IppJobSnapshot* out) const noexcept;
    bool active() const noexcept;
    void reset() noexcept;

private:
    static bool copy_text_(char* destination, size_t capacity,
                           const char* source, bool allow_empty) noexcept;
    static void copy_snapshot_(IppJobSnapshot* destination,
                               const IppJobSnapshot& source) noexcept;
    IppJobError check_id_(uint32_t id) const noexcept;

    IppJobSnapshot job_{};
    uint32_t next_id_ = 1;
    bool has_job_ = false;
};

}  // namespace airprint
}  // namespace ijpd

#endif  // IJPD_AIRPRINT_CORE_IPP_JOB_H
