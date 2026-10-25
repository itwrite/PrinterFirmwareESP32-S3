// ipp_print_job.cpp - Streaming Print-Job ingestion without document decoding.
#include "airprint_core/ipp_print_job.h"

#include <cstring>

#include "airprint_core/ipp_attributes.h"

namespace ijpd {
namespace airprint {
namespace {

bool copy_view(const IppAttributeView* attribute, char* output, size_t capacity,
               const char* fallback) noexcept {
    if (output == nullptr || capacity == 0) return false;
    if (attribute == nullptr) {
        if (fallback == nullptr) return false;
        const size_t length = std::strlen(fallback);
        if (length >= capacity) return false;
        std::memcpy(output, fallback, length + 1);
        return true;
    }
    if (attribute->value == nullptr || attribute->value_length == 0 ||
        attribute->value_length >= capacity) {
        return false;
    }
    std::memcpy(output, attribute->value, attribute->value_length);
    output[attribute->value_length] = '\0';
    return true;
}

bool equals_ascii_ci(const IppAttributeView* attribute, const char* expected) noexcept {
    if (attribute == nullptr || attribute->value == nullptr || expected == nullptr) {
        return false;
    }
    const size_t expected_length = std::strlen(expected);
    if (attribute->value_length != expected_length) return false;
    for (size_t i = 0; i < expected_length; ++i) {
        char actual = static_cast<char>(attribute->value[i]);
        char wanted = expected[i];
        if (actual >= 'A' && actual <= 'Z') actual = static_cast<char>(actual + ('a' - 'A'));
        if (wanted >= 'A' && wanted <= 'Z') wanted = static_cast<char>(wanted + ('a' - 'A'));
        if (actual != wanted) return false;
    }
    return true;
}

}  // namespace

const char* ipp_print_stream_error_name(IppPrintStreamError error) noexcept {
    switch (error) {
        case IppPrintStreamError::Ok: return "ok";
        case IppPrintStreamError::InvalidArgument: return "invalid_argument";
        case IppPrintStreamError::InvalidState: return "invalid_state";
        case IppPrintStreamError::MetadataTooLarge: return "metadata_too_large";
        case IppPrintStreamError::MalformedRequest: return "malformed_request";
        case IppPrintStreamError::UnsupportedOperation: return "unsupported_operation";
        case IppPrintStreamError::UnsupportedDocumentFormat:
            return "unsupported_document_format";
        case IppPrintStreamError::JobAlreadyRunning: return "job_already_running";
        case IppPrintStreamError::BackendError: return "backend_error";
        case IppPrintStreamError::BodyTooLarge: return "body_too_large";
        case IppPrintStreamError::EmptyDocument: return "empty_document";
        case IppPrintStreamError::IncompleteMetadata: return "incomplete_metadata";
    }
    return "unknown";
}

IppPrintJobStream::IppPrintJobStream(IppJobStore* jobs, const IppPrintBackend& backend,
                                     const IppPrintJobLimits& limits) noexcept
    : jobs_(jobs), backend_(backend), limits_(limits) {
    metadata_limit_ = limits_.max_metadata_bytes;
    if (metadata_limit_ == 0 || metadata_limit_ > sizeof(metadata_)) {
        metadata_limit_ = sizeof(metadata_);
    }
    reset();
}

void IppPrintJobStream::configure_limits(const IppPrintJobLimits& limits) noexcept {
    if (state_ != IppPrintStreamState::Idle) return;
    limits_ = limits;
    metadata_limit_ = limits_.max_metadata_bytes;
    if (metadata_limit_ == 0 || metadata_limit_ > sizeof(metadata_)) {
        metadata_limit_ = sizeof(metadata_);
    }
}

void IppPrintJobStream::configure_backend(const IppPrintBackend& backend) noexcept {
    if (state_ == IppPrintStreamState::Idle) backend_ = backend;
}

void IppPrintJobStream::reset() noexcept {
    state_ = IppPrintStreamState::Idle;
    metadata_length_ = 0;
    request_ = IppRequest{};
    job_ = IppJobSnapshot{};
    printer_uri_[0] = '\0';
    job_name_[0] = '\0';
    user_name_[0] = '\0';
    document_bytes_ = 0;
}

IppPrintFeedResult IppPrintJobStream::fail_(IppPrintStreamError error,
                                            size_t consumed) noexcept {
    state_ = IppPrintStreamState::Failed;
    return IppPrintFeedResult{error, consumed, false, false};
}

IppPrintStreamError IppPrintJobStream::map_job_error_(IppJobError error) const noexcept {
    switch (error) {
        case IppJobError::Ok: return IppPrintStreamError::Ok;
        case IppJobError::AlreadyActive: return IppPrintStreamError::JobAlreadyRunning;
        case IppJobError::InvalidArgument: return IppPrintStreamError::InvalidArgument;
        default: return IppPrintStreamError::BackendError;
    }
}

IppPrintStreamError IppPrintJobStream::copy_attribute_(const char* name, char* output,
                                                       size_t capacity,
                                                       const char* fallback) noexcept {
    return copy_view(request_.find_first(name), output, capacity, fallback)
               ? IppPrintStreamError::Ok
               : IppPrintStreamError::InvalidArgument;
}

IppPrintStreamError IppPrintJobStream::begin_job_(uint64_t now_ms) noexcept {
    if (jobs_ == nullptr || backend_.begin == nullptr || backend_.write == nullptr ||
        backend_.end == nullptr || backend_.cancel == nullptr ||
        limits_.printer_uri == nullptr || limits_.document_format == nullptr) {
        return IppPrintStreamError::InvalidArgument;
    }
    if (request_.operation != IppOperation::PrintJob) {
        return IppPrintStreamError::UnsupportedOperation;
    }
    bool format_supported = false;
    for (size_t i = 0; i < request_.attribute_count; ++i) {
        const IppAttributeView& attribute = request_.attributes[i];
        if (!attribute_name_equals(attribute, "document-format")) continue;
        if (equals_ascii_ci(&attribute, limits_.document_format) ||
            (equals_ascii_ci(&attribute, "application/octet-stream") &&
             limits_.document_format != nullptr &&
             std::strcmp(limits_.document_format, "image/urf") == 0)) {
            format_supported = true;
            break;
        }
    }
    if (!format_supported) {
        return IppPrintStreamError::UnsupportedDocumentFormat;
    }
    if (!copy_view(request_.find_first("printer-uri"), printer_uri_, sizeof(printer_uri_),
                   limits_.printer_uri) ||
        copy_attribute_("job-name", job_name_, sizeof(job_name_), "AirPrint Job") !=
            IppPrintStreamError::Ok ||
        copy_attribute_("requesting-user-name", user_name_, sizeof(user_name_),
                        "anonymous") != IppPrintStreamError::Ok) {
        return IppPrintStreamError::InvalidArgument;
    }
    const IppJobError created = jobs_->create(printer_uri_, job_name_, user_name_,
                                              limits_.document_format, now_ms, &job_);
    if (created != IppJobError::Ok) return map_job_error_(created);
    const IppJobError backend_begin = backend_.begin(backend_.user, request_, job_);
    if (backend_begin != IppJobError::Ok) {
        (void)jobs_->abort(job_.id, "backend-error", now_ms);
        return IppPrintStreamError::BackendError;
    }
    const IppJobError processing = jobs_->start_processing(job_.id, now_ms);
    if (processing != IppJobError::Ok) {
        (void)backend_.cancel(backend_.user);
        (void)jobs_->abort(job_.id, "job-state-error", now_ms);
        return map_job_error_(processing);
    }
    (void)jobs_->get(job_.id, &job_);
    state_ = IppPrintStreamState::ReceivingDocument;
    return IppPrintStreamError::Ok;
}

IppPrintStreamError IppPrintJobStream::write_document_(const uint8_t* data,
                                                       size_t length) noexcept {
    if (length == 0) return IppPrintStreamError::Ok;
    if (data == nullptr) return IppPrintStreamError::InvalidArgument;
    if (document_bytes_ > limits_.max_document_bytes ||
        static_cast<uint64_t>(length) > limits_.max_document_bytes - document_bytes_) {
        return IppPrintStreamError::BodyTooLarge;
    }
    if (backend_.write(backend_.user, data, length) != IppJobError::Ok) {
        return IppPrintStreamError::BackendError;
    }
    document_bytes_ += length;
    return IppPrintStreamError::Ok;
}

IppPrintFeedResult IppPrintJobStream::feed(const uint8_t* data, size_t length,
                                           uint64_t now_ms) noexcept {
    if (data == nullptr && length != 0) return fail_(IppPrintStreamError::InvalidArgument, 0);
    if (state_ == IppPrintStreamState::Idle) state_ = IppPrintStreamState::ReadingMetadata;
    if (state_ != IppPrintStreamState::ReadingMetadata &&
        state_ != IppPrintStreamState::ReceivingDocument) {
        return fail_(IppPrintStreamError::InvalidState, 0);
    }

    size_t consumed = 0;
    if (state_ == IppPrintStreamState::ReadingMetadata) {
        while (consumed < length) {
            if (metadata_length_ >= metadata_limit_) {
                return fail_(IppPrintStreamError::MetadataTooLarge, consumed);
            }
            metadata_[metadata_length_++] = data[consumed++];
            IppRequestPolicy policy{limits_.document_format, true, false, true};
            const IppRequestError parsed =
                parse_ipp_request(metadata_, metadata_length_, policy, &request_);
            if (parsed == IppRequestError::Truncated) continue;
            if (parsed != IppRequestError::Ok) {
                return fail_(parsed == IppRequestError::UnsupportedDocumentFormat
                                 ? IppPrintStreamError::UnsupportedDocumentFormat
                                 : IppPrintStreamError::MalformedRequest,
                             consumed);
            }
            const size_t prefix_length =
                static_cast<size_t>(request_.document_data - metadata_);
            const size_t document_length = metadata_length_ - prefix_length;
            const IppPrintStreamError started = begin_job_(now_ms);
            if (started != IppPrintStreamError::Ok) return fail_(started, consumed);
            if (document_length != 0) {
                const IppPrintStreamError written = write_document_(
                    request_.document_data, document_length);
                if (written != IppPrintStreamError::Ok) {
                    abort_backend_(now_ms, ipp_print_stream_error_name(written));
                    return fail_(written, consumed);
                }
            }
            return IppPrintFeedResult{IppPrintStreamError::Ok, consumed, true, false};
        }
        return IppPrintFeedResult{IppPrintStreamError::Ok, consumed, false, false};
    }

    const IppPrintStreamError written = write_document_(data, length);
    if (written != IppPrintStreamError::Ok) {
        abort_backend_(now_ms, ipp_print_stream_error_name(written));
        return fail_(written, 0);
    }
    return IppPrintFeedResult{IppPrintStreamError::Ok, length, true, false};
}

void IppPrintJobStream::abort_backend_(uint64_t now_ms, const char* reason) noexcept {
    if (backend_.cancel != nullptr) (void)backend_.cancel(backend_.user);
    if (jobs_ != nullptr && job_.id != 0) {
        (void)jobs_->abort(job_.id, reason, now_ms);
        (void)jobs_->get(job_.id, &job_);
    }
    state_ = IppPrintStreamState::Failed;
}

IppPrintStreamError IppPrintJobStream::finish(uint64_t now_ms) noexcept {
    if (state_ == IppPrintStreamState::ReadingMetadata) {
        state_ = IppPrintStreamState::Failed;
        return IppPrintStreamError::IncompleteMetadata;
    }
    if (state_ != IppPrintStreamState::ReceivingDocument) {
        return IppPrintStreamError::InvalidState;
    }
    if (document_bytes_ == 0) {
        abort_backend_(now_ms, "empty-document");
        return IppPrintStreamError::EmptyDocument;
    }
    if (backend_.end(backend_.user, true) != IppJobError::Ok) {
        abort_backend_(now_ms, "backend-error");
        return IppPrintStreamError::BackendError;
    }
    uint32_t pages_completed = 1;
    if (backend_.pages_completed != nullptr) {
        pages_completed = backend_.pages_completed(backend_.user);
        if (pages_completed == 0) {
            (void)jobs_->abort(job_.id, "backend-error", now_ms);
            state_ = IppPrintStreamState::Failed;
            return IppPrintStreamError::BackendError;
        }
    }
    if (jobs_->complete(job_.id, now_ms, pages_completed,
                        static_cast<uint32_t>((document_bytes_ + 1023) / 1024)) !=
            IppJobError::Ok) {
        abort_backend_(now_ms, "backend-error");
        return IppPrintStreamError::BackendError;
    }
    (void)jobs_->get(job_.id, &job_);
    state_ = IppPrintStreamState::Completed;
    return IppPrintStreamError::Ok;
}

IppPrintStreamError IppPrintJobStream::cancel(uint64_t now_ms) noexcept {
    if (state_ == IppPrintStreamState::Canceled) return IppPrintStreamError::Ok;
    if (state_ != IppPrintStreamState::ReceivingDocument &&
        state_ != IppPrintStreamState::ReadingMetadata) {
        return IppPrintStreamError::InvalidState;
    }
    if (backend_.cancel != nullptr &&
        backend_.cancel(backend_.user) != IppJobError::Ok) {
        if (jobs_ != nullptr && job_.id != 0) {
            (void)jobs_->abort(job_.id, "backend-error", now_ms);
            (void)jobs_->get(job_.id, &job_);
        }
        state_ = IppPrintStreamState::Failed;
        return IppPrintStreamError::BackendError;
    }
    if (jobs_ != nullptr && job_.id != 0 &&
        jobs_->cancel(job_.id, now_ms) != IppJobError::Ok) {
        state_ = IppPrintStreamState::Failed;
        (void)jobs_->get(job_.id, &job_);
        return IppPrintStreamError::BackendError;
    }
    state_ = IppPrintStreamState::Canceled;
    return IppPrintStreamError::Ok;
}

}  // namespace airprint
}  // namespace ijpd
