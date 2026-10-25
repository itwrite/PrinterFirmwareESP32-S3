// ipp_operations.cpp - First-slice IPP operation dispatcher.
#include "airprint_core/ipp_operations.h"

#include <cstring>

namespace ijpd {
namespace airprint {
namespace {

enum class JobSelectorResult : uint8_t {
    Ok = 0,
    BadRequest,
    NotFound,
};

IppError add_error_attribute(IppWriter& writer, const char* name) noexcept {
    if (name == nullptr || name[0] == '\0') return IppError::Ok;
    return writer.add_cstr(IppValueTag::Keyword, "unsupported", name);
}

IppDispatchError build_job_response(const IppRequest& request,
                                    IppStatus status,
                                    const IppJobSnapshot* job,
                                    uint8_t* output, size_t output_capacity,
                                    size_t* output_length) noexcept {
    IppWriter writer(output, output_capacity);
    if (writer.begin_response(request.header.version, status, request.header.request_id) !=
            IppError::Ok ||
        writer.begin_group(IppGroupTag::OperationAttributes) != IppError::Ok ||
        writer.add_cstr(IppValueTag::Charset, "attributes-charset", "utf-8") !=
            IppError::Ok ||
        writer.add_cstr(IppValueTag::NaturalLanguage,
                        "attributes-natural-language", "en") != IppError::Ok) {
        return IppDispatchError::OutputTooSmall;
    }
    if (job != nullptr) {
        if (writer.begin_group(IppGroupTag::JobAttributes) != IppError::Ok ||
            writer.add_integer("job-id", static_cast<int32_t>(job->id)) != IppError::Ok ||
            writer.add_cstr(IppValueTag::Uri, "job-uri", job->job_uri) != IppError::Ok ||
            writer.add_cstr(IppValueTag::Uri, "job-printer-uri", job->printer_uri) != IppError::Ok ||
            writer.add_cstr(IppValueTag::NameWithoutLanguage, "job-name", job->job_name) !=
                IppError::Ok ||
            writer.add_cstr(IppValueTag::NameWithoutLanguage,
                            "job-originating-user-name", job->user_name) != IppError::Ok ||
            writer.add_cstr(IppValueTag::MimeMediaType, "document-format",
                            job->document_format) != IppError::Ok ||
            writer.add_enum("job-state", ipp_job_state_value(job->state)) !=
                IppError::Ok ||
            writer.add_cstr(IppValueTag::Keyword, "job-state-reasons",
                            job->state_reason) != IppError::Ok ||
            writer.add_integer("pages-completed",
                               static_cast<int32_t>(job->pages_completed)) != IppError::Ok ||
            writer.add_integer("job-k-octets",
                               static_cast<int32_t>(job->job_k_octets)) != IppError::Ok) {
            return IppDispatchError::OutputTooSmall;
        }
    }
    if (writer.end_attributes() != IppError::Ok) return IppDispatchError::OutputTooSmall;
    *output_length = writer.length();
    return IppDispatchError::Ok;
}

JobSelectorResult resolve_job_selector(const IppRequest& request, IppJobStore* jobs,
                                       IppJobSnapshot* snapshot) noexcept {
    if (jobs == nullptr || snapshot == nullptr) return JobSelectorResult::BadRequest;
    const IppAttributeView* id = request.find_first("job-id");
    const IppAttributeView* uri = request.find_first("job-uri");
    if (id == nullptr && uri == nullptr) return JobSelectorResult::BadRequest;

    uint32_t requested_id = 0;
    if (id != nullptr) {
        if (id->value == nullptr || id->value_length != 4) {
            return JobSelectorResult::BadRequest;
        }
        requested_id = (static_cast<uint32_t>(id->value[0]) << 24) |
                       (static_cast<uint32_t>(id->value[1]) << 16) |
                       (static_cast<uint32_t>(id->value[2]) << 8) |
                       static_cast<uint32_t>(id->value[3]);
    }

    if (jobs->get_last(snapshot) != IppJobError::Ok) return JobSelectorResult::NotFound;
    if (id != nullptr && snapshot->id != requested_id) return JobSelectorResult::NotFound;
    if (uri != nullptr &&
        (uri->value == nullptr || uri->value_length != std::strlen(snapshot->job_uri) ||
         std::memcmp(uri->value, snapshot->job_uri, uri->value_length) != 0)) {
        return JobSelectorResult::NotFound;
    }
    return JobSelectorResult::Ok;
}

}  // namespace

const char* ipp_dispatch_error_name(IppDispatchError error) noexcept {
    switch (error) {
        case IppDispatchError::Ok: return "ok";
        case IppDispatchError::InvalidArgument: return "invalid_argument";
        case IppDispatchError::OutputTooSmall: return "output_too_small";
        case IppDispatchError::UnsupportedOperation: return "unsupported_operation";
    }
    return "unknown";
}

IppDispatchError build_ipp_error_response(
    const IppRequest& request, IppStatus status, const char* unsupported_attribute,
    uint8_t* output, size_t output_capacity, size_t* output_length) noexcept {
    if (output == nullptr || output_length == nullptr) return IppDispatchError::InvalidArgument;
    *output_length = 0;
    IppWriter writer(output, output_capacity);
    if (writer.begin_response(request.header.version, status, request.header.request_id) !=
            IppError::Ok ||
        writer.begin_group(IppGroupTag::OperationAttributes) != IppError::Ok ||
        writer.add_cstr(IppValueTag::Charset, "attributes-charset", "utf-8") !=
            IppError::Ok ||
        writer.add_cstr(IppValueTag::NaturalLanguage,
                        "attributes-natural-language", "en") != IppError::Ok ||
        writer.begin_group(IppGroupTag::UnsupportedAttributes) != IppError::Ok ||
        add_error_attribute(writer, unsupported_attribute) != IppError::Ok ||
        writer.end_attributes() != IppError::Ok) {
        return IppDispatchError::OutputTooSmall;
    }
    *output_length = writer.length();
    return IppDispatchError::Ok;
}

IppDispatchError build_ipp_job_response(
    const IppRequest& request, IppStatus status, const IppJobSnapshot* job,
    uint8_t* output, size_t output_capacity, size_t* output_length) noexcept {
    if (output == nullptr || output_length == nullptr) return IppDispatchError::InvalidArgument;
    *output_length = 0;
    return build_job_response(request, status, job, output, output_capacity, output_length);
}

IppDispatchError dispatch_ipp_request(
    const IppRequest& request, const AirPrintCapabilities& capabilities,
    uint8_t* output, size_t output_capacity, size_t* output_length) noexcept {
    if (output == nullptr || output_length == nullptr) return IppDispatchError::InvalidArgument;
    *output_length = 0;

    if (request.error != IppRequestError::Ok) {
        const IppStatus status = request.status == IppStatus::SuccessfulOk
                                     ? IppStatus::ClientErrorBadRequest
                                     : request.status;
        const char* attribute = request.error_attribute_length != 0
                                    ? "request-attributes"
                                    : nullptr;
        return build_ipp_error_response(request, status, attribute, output,
                                        output_capacity, output_length);
    }

    switch (request.operation) {
        case IppOperation::GetPrinterAttributes: {
            const AirPrintResponseError response = build_get_printer_attributes_response(
                request, capabilities, output, output_capacity, output_length);
            if (response == AirPrintResponseError::Ok) return IppDispatchError::Ok;
            if (response == AirPrintResponseError::OutputTooSmall) {
                return IppDispatchError::OutputTooSmall;
            }
            if (response == AirPrintResponseError::InvalidCapabilities) {
                return build_ipp_error_response(
                    request, IppStatus::ServerErrorServiceUnavailable, nullptr,
                    output, output_capacity, output_length);
            }
            return IppDispatchError::InvalidArgument;
        }
        case IppOperation::PrintJob:
        case IppOperation::ValidateJob:
        case IppOperation::CancelJob:
        case IppOperation::GetJobAttributes:
        case IppOperation::GetJobs:
            return build_ipp_error_response(
                request, IppStatus::ServerErrorOperationNotSupported, nullptr,
                output, output_capacity, output_length);
    }
    return build_ipp_error_response(request, IppStatus::ServerErrorOperationNotSupported,
                                    nullptr, output, output_capacity, output_length);
}

IppDispatchError dispatch_ipp_request_with_jobs(
    const IppRequest& request, const AirPrintCapabilities& capabilities,
    IppJobStore* jobs, uint64_t now_ms,
    uint8_t* output, size_t output_capacity, size_t* output_length) noexcept {
    if (jobs == nullptr || output == nullptr || output_length == nullptr) {
        return IppDispatchError::InvalidArgument;
    }
    *output_length = 0;
    if (request.error != IppRequestError::Ok) {
        const IppStatus status = request.status == IppStatus::SuccessfulOk
                                     ? IppStatus::ClientErrorBadRequest
                                     : request.status;
        return build_ipp_error_response(request, status, nullptr, output,
                                        output_capacity, output_length);
    }
    switch (request.operation) {
        case IppOperation::GetPrinterAttributes:
            return dispatch_ipp_request(request, capabilities, output, output_capacity,
                                        output_length);
        case IppOperation::ValidateJob:
            return build_job_response(request, IppStatus::SuccessfulOk, nullptr,
                                       output, output_capacity, output_length);
        case IppOperation::GetJobAttributes: {
            IppJobSnapshot snapshot{};
            const JobSelectorResult selected = resolve_job_selector(request, jobs, &snapshot);
            if (selected == JobSelectorResult::BadRequest) {
                return build_ipp_error_response(
                    request, IppStatus::ClientErrorBadRequest, "job-id", output,
                    output_capacity, output_length);
            }
            if (selected == JobSelectorResult::NotFound) {
                return build_ipp_error_response(
                    request, IppStatus::ClientErrorNotFound, "job-id", output,
                    output_capacity, output_length);
            }
            return build_job_response(request, IppStatus::SuccessfulOk, &snapshot,
                                      output, output_capacity, output_length);
        }
        case IppOperation::GetJobs: {
            IppJobSnapshot snapshot{};
            const IppJobError error = jobs->get_last(&snapshot);
            return build_job_response(request,
                                      error == IppJobError::Ok
                                          ? IppStatus::SuccessfulOk
                                          : IppStatus::SuccessfulOk,
                                      error == IppJobError::Ok ? &snapshot : nullptr,
                                      output, output_capacity, output_length);
        }
        case IppOperation::CancelJob: {
            IppJobSnapshot selected_job{};
            const JobSelectorResult selected =
                resolve_job_selector(request, jobs, &selected_job);
            if (selected == JobSelectorResult::BadRequest) {
                return build_ipp_error_response(
                    request, IppStatus::ClientErrorBadRequest, "job-id", output,
                    output_capacity, output_length);
            }
            if (selected == JobSelectorResult::NotFound) {
                return build_ipp_error_response(
                    request, IppStatus::ClientErrorNotFound, "job-id", output,
                    output_capacity, output_length);
            }
            const uint32_t job_id = selected_job.id;
            const IppJobError error = jobs->cancel(job_id, now_ms);
            if (error == IppJobError::NoActiveJob || error == IppJobError::IdMismatch) {
                return build_ipp_error_response(
                    request, IppStatus::ClientErrorNotFound, "job-id", output,
                    output_capacity, output_length);
            }
            if (error != IppJobError::Ok) {
                return build_ipp_error_response(
                    request, IppStatus::ClientErrorNotPossible, "job-id", output,
                    output_capacity, output_length);
            }
            IppJobSnapshot snapshot{};
            (void)jobs->get(job_id, &snapshot);
            return build_job_response(request, IppStatus::SuccessfulOk, &snapshot,
                                      output, output_capacity, output_length);
        }
        case IppOperation::PrintJob:
            return build_ipp_error_response(
                request, IppStatus::ServerErrorOperationNotSupported, nullptr,
                output, output_capacity, output_length);
    }
    return build_ipp_error_response(request, IppStatus::ServerErrorOperationNotSupported,
                                    nullptr, output, output_capacity, output_length);
}

}  // namespace airprint
}  // namespace ijpd
