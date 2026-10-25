// ipp_job.cpp - Single-active bounded IPP Job lifecycle.
#include "airprint_core/ipp_job.h"

#include <cstdio>
#include <cstring>

namespace ijpd {
namespace airprint {

const char* ipp_job_state_name(IppJobState state) noexcept {
    switch (state) {
        case IppJobState::Idle: return "idle";
        case IppJobState::Pending: return "pending";
        case IppJobState::Processing: return "processing";
        case IppJobState::Completed: return "completed";
        case IppJobState::Canceled: return "canceled";
        case IppJobState::Aborted: return "aborted";
    }
    return "unknown";
}

int32_t ipp_job_state_value(IppJobState state) noexcept {
    switch (state) {
        case IppJobState::Pending: return 3;
        case IppJobState::Processing: return 5;
        case IppJobState::Canceled: return 7;
        case IppJobState::Aborted: return 8;
        case IppJobState::Completed: return 9;
        case IppJobState::Idle: return 0;
    }
    return 0;
}

const char* ipp_job_error_name(IppJobError error) noexcept {
    switch (error) {
        case IppJobError::Ok: return "ok";
        case IppJobError::InvalidArgument: return "invalid_argument";
        case IppJobError::AlreadyActive: return "already_active";
        case IppJobError::NoActiveJob: return "no_active_job";
        case IppJobError::InvalidState: return "invalid_state";
        case IppJobError::IdMismatch: return "id_mismatch";
        case IppJobError::OutputTooSmall: return "output_too_small";
    }
    return "unknown";
}

bool IppJobStore::copy_text_(char* destination, size_t capacity,
                             const char* source, bool allow_empty) noexcept {
    if (destination == nullptr || capacity == 0 || source == nullptr) return false;
    const size_t length = std::strlen(source);
    if ((!allow_empty && length == 0) || length >= capacity) return false;
    std::memcpy(destination, source, length + 1);
    return true;
}

void IppJobStore::copy_snapshot_(IppJobSnapshot* destination,
                                 const IppJobSnapshot& source) noexcept {
    if (destination != nullptr) *destination = source;
}

IppJobError IppJobStore::check_id_(uint32_t id) const noexcept {
    if (!has_job_) return IppJobError::NoActiveJob;
    return job_.id == id ? IppJobError::Ok : IppJobError::IdMismatch;
}

IppJobError IppJobStore::create(const char* printer_uri, const char* job_name,
                                const char* user_name,
                                const char* document_format, uint64_t now_ms,
                                IppJobSnapshot* out) noexcept {
    if (printer_uri == nullptr || job_name == nullptr || user_name == nullptr ||
        document_format == nullptr ||
        out == nullptr || job_name[0] == '\0' || user_name[0] == '\0' ||
        document_format[0] == '\0' || printer_uri[0] == '\0') {
        return IppJobError::InvalidArgument;
    }
    if (has_job_ &&
        (job_.state == IppJobState::Pending || job_.state == IppJobState::Processing)) {
        return IppJobError::AlreadyActive;
    }
    job_ = IppJobSnapshot{};
    job_.id = next_id_++;
    if (job_.id == 0) job_.id = next_id_++;
    const int job_id_length = std::snprintf(job_.job_id, sizeof(job_.job_id), "%lu",
                                            static_cast<unsigned long>(job_.id));
    const int job_uri_length = std::snprintf(job_.job_uri, sizeof(job_.job_uri),
                                             "%s/job/%lu", printer_uri,
                                             static_cast<unsigned long>(job_.id));
    if (job_id_length < 0 || static_cast<size_t>(job_id_length) >= sizeof(job_.job_id) ||
        job_uri_length < 0 || static_cast<size_t>(job_uri_length) >= sizeof(job_.job_uri)) {
        job_ = IppJobSnapshot{};
        return IppJobError::InvalidArgument;
    }
    if (!copy_text_(job_.printer_uri, sizeof(job_.printer_uri), printer_uri, false) ||
        !copy_text_(job_.job_name, sizeof(job_.job_name), job_name, false) ||
        !copy_text_(job_.user_name, sizeof(job_.user_name), user_name, false) ||
        !copy_text_(job_.document_format, sizeof(job_.document_format),
                    document_format, false)) {
        job_ = IppJobSnapshot{};
        return IppJobError::InvalidArgument;
    }
    job_.state = IppJobState::Pending;
    job_.created_ms = now_ms;
    has_job_ = true;
    *out = job_;
    return IppJobError::Ok;
}

IppJobError IppJobStore::start_processing(uint32_t id, uint64_t now_ms) noexcept {
    const IppJobError id_error = check_id_(id);
    if (id_error != IppJobError::Ok) return id_error;
    if (job_.state != IppJobState::Pending) return IppJobError::InvalidState;
    job_.state = IppJobState::Processing;
    job_.processing_ms = now_ms;
    return IppJobError::Ok;
}

IppJobError IppJobStore::complete(uint32_t id, uint64_t now_ms,
                                  uint32_t pages_completed,
                                  uint32_t job_k_octets) noexcept {
    const IppJobError id_error = check_id_(id);
    if (id_error != IppJobError::Ok) return id_error;
    if (job_.state != IppJobState::Processing) return IppJobError::InvalidState;
    job_.state = IppJobState::Completed;
    job_.state_reason[0] = 'n';
    std::strcpy(job_.state_reason, "none");
    job_.pages_completed = pages_completed;
    job_.job_k_octets = job_k_octets;
    job_.completed_ms = now_ms;
    return IppJobError::Ok;
}

IppJobError IppJobStore::abort(uint32_t id, const char* reason,
                               uint64_t now_ms) noexcept {
    const IppJobError id_error = check_id_(id);
    if (id_error != IppJobError::Ok) return id_error;
    if (job_.state != IppJobState::Pending && job_.state != IppJobState::Processing) {
        return IppJobError::InvalidState;
    }
    if (!copy_text_(job_.state_reason, sizeof(job_.state_reason),
                    reason != nullptr ? reason : "aborted-by-system", false)) {
        return IppJobError::InvalidArgument;
    }
    job_.state = IppJobState::Aborted;
    job_.completed_ms = now_ms;
    return IppJobError::Ok;
}

IppJobError IppJobStore::cancel(uint32_t id, uint64_t now_ms) noexcept {
    const IppJobError id_error = check_id_(id);
    if (id_error != IppJobError::Ok) return id_error;
    if (job_.state == IppJobState::Canceled) return IppJobError::Ok;
    if (job_.state != IppJobState::Pending && job_.state != IppJobState::Processing) {
        return IppJobError::InvalidState;
    }
    std::strcpy(job_.state_reason, "job-canceled-by-user");
    job_.state = IppJobState::Canceled;
    job_.completed_ms = now_ms;
    return IppJobError::Ok;
}

IppJobError IppJobStore::get(uint32_t id, IppJobSnapshot* out) const noexcept {
    if (out == nullptr) return IppJobError::InvalidArgument;
    const IppJobError id_error = check_id_(id);
    if (id_error != IppJobError::Ok) return id_error;
    copy_snapshot_(out, job_);
    return IppJobError::Ok;
}

IppJobError IppJobStore::get_active(IppJobSnapshot* out) const noexcept {
    if (out == nullptr) return IppJobError::InvalidArgument;
    if (!has_job_ ||
        (job_.state != IppJobState::Pending && job_.state != IppJobState::Processing)) {
        return IppJobError::NoActiveJob;
    }
    copy_snapshot_(out, job_);
    return IppJobError::Ok;
}

IppJobError IppJobStore::get_last(IppJobSnapshot* out) const noexcept {
    if (out == nullptr) return IppJobError::InvalidArgument;
    if (!has_job_) return IppJobError::NoActiveJob;
    copy_snapshot_(out, job_);
    return IppJobError::Ok;
}

bool IppJobStore::active() const noexcept {
    return has_job_ &&
           (job_.state == IppJobState::Pending || job_.state == IppJobState::Processing);
}

void IppJobStore::reset() noexcept {
    job_ = IppJobSnapshot{};
    has_job_ = false;
}

}  // namespace airprint
}  // namespace ijpd
