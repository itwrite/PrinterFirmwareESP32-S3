// ipp_request.cpp - Bounded IPP request validation.
#include "airprint_core/ipp_request.h"

#include <cstring>

#include "airprint_core/ipp_attributes.h"

namespace ijpd {
namespace airprint {
namespace {

bool equals_ascii_ci(const uint8_t* value, size_t length, const char* expected) noexcept {
    if (value == nullptr || expected == nullptr) return false;
    const size_t expected_length = std::strlen(expected);
    if (length != expected_length) return false;
    for (size_t i = 0; i < length; ++i) {
        char actual = static_cast<char>(value[i]);
        char wanted = expected[i];
        if (actual >= 'A' && actual <= 'Z') actual = static_cast<char>(actual + ('a' - 'A'));
        if (wanted >= 'A' && wanted <= 'Z') wanted = static_cast<char>(wanted + ('a' - 'A'));
        if (actual != wanted) return false;
    }
    return true;
}

bool equals_ascii(const IppAttributeView& attribute, const char* expected) noexcept {
    return equals_ascii_ci(attribute.value, attribute.value_length, expected);
}

bool is_ipp_uri(const IppAttributeView* attribute) noexcept {
    return attribute != nullptr && attribute->value_length >= 6 &&
           std::memcmp(attribute->value, "ipp://", 6) == 0;
}

bool document_format_matches(const IppRequest& request,
                             const IppRequestPolicy& policy) noexcept {
    if (policy.document_format == nullptr) return true;
    for (size_t i = 0; i < request.attribute_count; ++i) {
        const IppAttributeView& attribute = request.attributes[i];
        if (!attribute_name_equals(attribute, "document-format")) continue;
        if (equals_ascii(attribute, policy.document_format)) return true;
        // IPP permits application/octet-stream when the document format is
        // identified from the document bytes. The URF decoder remains the gate.
        if (equals_ascii(attribute, "application/octet-stream") &&
            std::strcmp(policy.document_format, "image/urf") == 0) {
            return true;
        }
    }
    return false;
}

bool name_is(const IppAttributeView& attribute, const char* name) noexcept {
    return attribute_name_equals(attribute, name);
}

bool is_supported_version(IppVersion version) noexcept {
    if (version.major == 1) return version.minor <= 1;
    if (version.major == 2) return version.minor <= 1;
    return false;
}

bool operation_from_code(uint16_t code, IppOperation* out) noexcept {
    if (out == nullptr) return false;
    switch (code) {
        case static_cast<uint16_t>(IppOperation::PrintJob):
            *out = IppOperation::PrintJob;
            return true;
        case static_cast<uint16_t>(IppOperation::ValidateJob):
            *out = IppOperation::ValidateJob;
            return true;
        case static_cast<uint16_t>(IppOperation::CancelJob):
            *out = IppOperation::CancelJob;
            return true;
        case static_cast<uint16_t>(IppOperation::GetJobAttributes):
            *out = IppOperation::GetJobAttributes;
            return true;
        case static_cast<uint16_t>(IppOperation::GetJobs):
            *out = IppOperation::GetJobs;
            return true;
        case static_cast<uint16_t>(IppOperation::GetPrinterAttributes):
            *out = IppOperation::GetPrinterAttributes;
            return true;
        default:
            return false;
    }
}

IppRequestError fail(IppRequest* out, IppRequestError error,
                     const IppAttributeView* attribute = nullptr) noexcept {
    out->error = error;
    out->status = ipp_request_error_status(error);
    if (attribute != nullptr) {
        out->error_attribute = attribute->name;
        out->error_attribute_length = attribute->name_length;
    }
    return error;
}

bool operation_attribute_allowed(const IppAttributeView& attribute,
                                 IppOperation operation) noexcept {
    if (attribute.group != IppGroupTag::OperationAttributes) return true;
    if (name_is(attribute, "attributes-charset")) return attribute.value_tag == IppValueTag::Charset;
    if (name_is(attribute, "attributes-natural-language")) {
        return attribute.value_tag == IppValueTag::NaturalLanguage;
    }
    if (name_is(attribute, "printer-uri") || name_is(attribute, "job-uri")) {
        return attribute.value_tag == IppValueTag::Uri;
    }
    if (name_is(attribute, "requesting-user-name") || name_is(attribute, "job-name")) {
        return attribute.value_tag == IppValueTag::NameWithoutLanguage;
    }
    if (name_is(attribute, "document-format")) {
        return attribute.value_tag == IppValueTag::MimeMediaType;
    }
    if (name_is(attribute, "document-format-version")) {
        return attribute.value_tag == IppValueTag::TextWithoutLanguage;
    }
    if (name_is(attribute, "ipp-attribute-fidelity")) {
        return attribute.value_tag == IppValueTag::Boolean && attribute.value_length == 1;
    }
    if (name_is(attribute, "requested-attributes")) {
        return attribute.value_tag == IppValueTag::Keyword;
    }
    if (name_is(attribute, "job-id")) {
        return attribute.value_tag == IppValueTag::Integer && attribute.value_length == 4;
    }
    (void)operation;
    return true;  // Unknown operation attributes may be safely ignored by the caller.
}

bool is_multi_value(const IppAttributeView& attribute) noexcept {
    return name_is(attribute, "requested-attributes") ||
           name_is(attribute, "which-jobs") || name_is(attribute, "my-jobs");
}

IppRequestError require_attribute(const IppRequest& request, const char* name,
                                  IppRequest* out) noexcept {
    if (!request.has(name)) return fail(out, IppRequestError::MissingAttribute);
    return IppRequestError::Ok;
}

IppRequestError validate_semantics(const IppRequestPolicy& policy,
                                   IppRequest* out) noexcept {
    const IppRequest& request = *out;
    if (require_attribute(request, "attributes-charset", out) != IppRequestError::Ok ||
        require_attribute(request, "attributes-natural-language", out) != IppRequestError::Ok) {
        return out->error;
    }

    const IppAttributeView* charset = request.find_first("attributes-charset");
    const IppAttributeView* language = request.find_first("attributes-natural-language");
    if (charset == nullptr || language == nullptr || !equals_ascii(*charset, "utf-8") ||
        language->value_length == 0) {
        return fail(out, IppRequestError::InvalidAttributeValue,
                    charset != nullptr ? charset : language);
    }

    const bool needs_printer_selector = request.operation == IppOperation::PrintJob ||
                                        request.operation == IppOperation::ValidateJob ||
                                        request.operation == IppOperation::GetJobs ||
                                        request.operation == IppOperation::GetPrinterAttributes;
    const bool needs_job_selector = request.operation == IppOperation::CancelJob ||
                                    request.operation == IppOperation::GetJobAttributes;
    if (needs_printer_selector) {
        const IppAttributeView* printer_uri = request.find_first("printer-uri");
        if (printer_uri == nullptr) return fail(out, IppRequestError::MissingAttribute);
        if (policy.require_ipp_uri && !is_ipp_uri(printer_uri)) {
            return fail(out, IppRequestError::InvalidAttributeValue, printer_uri);
        }
    }

    const bool needs_document_format = request.operation == IppOperation::PrintJob ||
                                       request.operation == IppOperation::ValidateJob;
    const IppAttributeView* document_format = request.find_first("document-format");
    if (needs_document_format && document_format == nullptr) {
        return fail(out, IppRequestError::MissingAttribute);
    }
    if (document_format != nullptr && policy.document_format != nullptr &&
        !document_format_matches(request, policy)) {
        return fail(out, IppRequestError::UnsupportedDocumentFormat, document_format);
    }

    if (needs_job_selector) {
        if (!request.has("job-id") && !request.has("job-uri")) {
            return fail(out, IppRequestError::MissingAttribute);
        }
        const IppAttributeView* job_uri = request.find_first("job-uri");
        if (policy.require_ipp_uri && job_uri != nullptr && !is_ipp_uri(job_uri)) {
            return fail(out, IppRequestError::InvalidAttributeValue, job_uri);
        }
    }

    if (request.operation == IppOperation::PrintJob) {
        if (policy.require_document && request.document_length == 0) {
            return fail(out, IppRequestError::MissingDocument);
        }
    } else if (request.document_length != 0) {
        return fail(out, IppRequestError::UnexpectedDocument);
    }
    return IppRequestError::Ok;
}

}  // namespace

const char* ipp_request_error_name(IppRequestError error) noexcept {
    switch (error) {
        case IppRequestError::Ok: return "ok";
        case IppRequestError::InvalidArgument: return "invalid_argument";
        case IppRequestError::Truncated: return "truncated";
        case IppRequestError::UnsupportedVersion: return "unsupported_version";
        case IppRequestError::UnsupportedOperation: return "unsupported_operation";
        case IppRequestError::InvalidGroupOrder: return "invalid_group_order";
        case IppRequestError::TooManyAttributes: return "too_many_attributes";
        case IppRequestError::MissingAttribute: return "missing_attribute";
        case IppRequestError::DuplicateAttribute: return "duplicate_attribute";
        case IppRequestError::InvalidAttributeTag: return "invalid_attribute_tag";
        case IppRequestError::InvalidAttributeValue: return "invalid_attribute_value";
        case IppRequestError::MissingDocument: return "missing_document";
        case IppRequestError::UnexpectedDocument: return "unexpected_document";
        case IppRequestError::UnsupportedDocumentFormat:
            return "unsupported_document_format";
    }
    return "unknown";
}

IppStatus ipp_request_error_status(IppRequestError error) noexcept {
    switch (error) {
        case IppRequestError::UnsupportedVersion:
            return IppStatus::ServerErrorVersionNotSupported;
        case IppRequestError::UnsupportedOperation:
            return IppStatus::ServerErrorOperationNotSupported;
        case IppRequestError::TooManyAttributes:
            return IppStatus::ClientErrorRequestEntityTooLarge;
        case IppRequestError::UnsupportedDocumentFormat:
            return IppStatus::ClientErrorDocumentFormatNotSupported;
        case IppRequestError::MissingDocument:
        case IppRequestError::UnexpectedDocument:
            return IppStatus::ClientErrorDocumentFormatError;
        case IppRequestError::Ok:
            return IppStatus::SuccessfulOk;
        default:
            return IppStatus::ClientErrorBadRequest;
    }
}

const IppAttributeView* IppRequest::find_first(const char* name) const noexcept {
    if (name == nullptr) return nullptr;
    for (size_t i = 0; i < attribute_count; ++i) {
        if (attribute_name_equals(attributes[i], name)) return &attributes[i];
    }
    return nullptr;
}

size_t IppRequest::count(const char* name) const noexcept {
    if (name == nullptr) return 0;
    size_t result = 0;
    for (size_t i = 0; i < attribute_count; ++i) {
        if (attribute_name_equals(attributes[i], name)) ++result;
    }
    return result;
}

IppRequestError parse_ipp_request(const uint8_t* data, size_t length,
                                  const IppRequestPolicy& policy,
                                  IppRequest* out) noexcept {
    if (out == nullptr || data == nullptr) return IppRequestError::InvalidArgument;
    *out = IppRequest{};

    IppReader reader;
    const IppError begin_error = reader.begin(data, length);
    if (begin_error != IppError::Ok) {
        return fail(out, begin_error == IppError::Truncated ? IppRequestError::Truncated
                                                             : IppRequestError::InvalidArgument);
    }
    out->header = reader.header();
    if (!is_supported_version(out->header.version)) {
        return fail(out, IppRequestError::UnsupportedVersion);
    }
    if (!operation_from_code(out->header.code, &out->operation)) {
        return fail(out, IppRequestError::UnsupportedOperation);
    }

    bool operation_group_seen = false;
    bool job_group_seen = false;
    IppGroupTag current_group = IppGroupTag::None;
    for (;;) {
        const IppReadResult result = reader.next();
        if (result.error != IppError::Ok) {
            return fail(out, result.error == IppError::Truncated
                                ? IppRequestError::Truncated
                                : IppRequestError::InvalidArgument);
        }
        if (result.kind == IppReadKind::EndOfAttributes) break;
        if (result.kind == IppReadKind::GroupStart) {
            current_group = result.group;
            if (!operation_group_seen) {
                if (current_group != IppGroupTag::OperationAttributes) {
                    return fail(out, IppRequestError::InvalidGroupOrder);
                }
                operation_group_seen = true;
            } else if (current_group == IppGroupTag::OperationAttributes ||
                       current_group == IppGroupTag::UnsupportedAttributes ||
                       current_group == IppGroupTag::PrinterAttributes ||
                       current_group == IppGroupTag::SubscriptionAttributes ||
                       current_group == IppGroupTag::EventNotificationAttributes) {
                return fail(out, IppRequestError::InvalidGroupOrder);
            } else if (current_group == IppGroupTag::JobAttributes) {
                if (out->operation != IppOperation::PrintJob &&
                    out->operation != IppOperation::ValidateJob) {
                    return fail(out, IppRequestError::InvalidGroupOrder);
                }
                if (job_group_seen) return fail(out, IppRequestError::InvalidGroupOrder);
                job_group_seen = true;
            }
            continue;
        }
        if (current_group == IppGroupTag::None || !operation_group_seen) {
            return fail(out, IppRequestError::InvalidGroupOrder);
        }
        if (out->attribute_count >= kMaxRequestAttributes) {
            return fail(out, IppRequestError::TooManyAttributes);
        }
        const IppAttributeView& attribute = result.attribute;
        if (attribute.group == IppGroupTag::OperationAttributes &&
            !operation_attribute_allowed(attribute, out->operation)) {
            return fail(out, IppRequestError::InvalidAttributeTag, &attribute);
        }
        if (attribute.group == IppGroupTag::OperationAttributes &&
            !attribute.continuation && !is_multi_value(attribute)) {
            for (size_t i = 0; i < out->attribute_count; ++i) {
                if (out->attributes[i].group == IppGroupTag::OperationAttributes &&
                    out->attributes[i].name_length == attribute.name_length &&
                    std::memcmp(out->attributes[i].name, attribute.name,
                                attribute.name_length) == 0 &&
                    !((policy.allow_duplicate_document_format &&
                       name_is(attribute, "document-format")) ||
                      (out->operation == IppOperation::PrintJob &&
                       name_is(attribute, "requesting-user-name")))) {
                    return fail(out, IppRequestError::DuplicateAttribute, &attribute);
                }
            }
        }
        out->attributes[out->attribute_count++] = attribute;
    }

    if (!operation_group_seen) return fail(out, IppRequestError::InvalidGroupOrder);
    out->document_data = reader.document_data();
    out->document_length = reader.document_length();
    return validate_semantics(policy, out);
}

}  // namespace airprint
}  // namespace ijpd
