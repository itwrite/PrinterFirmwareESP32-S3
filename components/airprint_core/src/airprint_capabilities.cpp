// airprint_capabilities.cpp - Truthful Get-Printer-Attributes response builder.
#include "airprint_core/airprint_capabilities.h"

#include <cstring>

#include "airprint_core/ipp_attributes.h"

namespace ijpd {
namespace airprint {
namespace {

bool requested_attribute(const IppRequest& request, const char* name) noexcept {
    const size_t count = request.count("requested-attributes");
    if (count == 0) return true;
    for (size_t i = 0; i < request.attribute_count; ++i) {
        const IppAttributeView& attribute = request.attributes[i];
        if (!attribute_name_equals(attribute, "requested-attributes") ||
            attribute.value_tag != IppValueTag::Keyword) {
            continue;
        }
        const size_t name_length = std::strlen(name);
        if ((attribute.value_length == 3 &&
             std::memcmp(attribute.value, "all", 3) == 0) ||
            (attribute.value_length == name_length &&
             std::memcmp(attribute.value, name, name_length) == 0)) {
            return true;
        }
    }
    return false;
}

IppError add_keyword_values(IppWriter& writer, const char* name,
                            const char* const* values, size_t count) noexcept {
    if (count == 0) return IppError::InvalidArgument;
    IppError error = writer.add_cstr(IppValueTag::Keyword, name, values[0]);
    for (size_t i = 1; error == IppError::Ok && i < count; ++i) {
        const char* value = values[i];
        error = writer.add_continuation(
            IppValueTag::Keyword, reinterpret_cast<const uint8_t*>(value),
            std::strlen(value));
    }
    return error;
}

IppError add_enum_values(IppWriter& writer, const char* name,
                         const int32_t* values, size_t count) noexcept {
    if (count == 0) return IppError::InvalidArgument;
    IppError error = writer.add_enum(name, values[0]);
    for (size_t i = 1; error == IppError::Ok && i < count; ++i) {
        uint8_t value[4] = {
            static_cast<uint8_t>(static_cast<uint32_t>(values[i]) >> 24),
            static_cast<uint8_t>(static_cast<uint32_t>(values[i]) >> 16),
            static_cast<uint8_t>(static_cast<uint32_t>(values[i]) >> 8),
            static_cast<uint8_t>(values[i]),
        };
        error = writer.add_continuation(IppValueTag::Enum, value, sizeof(value));
    }
    return error;
}

IppError add_resolution_values(IppWriter& writer, const char* name,
                               uint16_t dpi_x, uint16_t dpi_y) noexcept {
    return writer.add_resolution(name, dpi_x, dpi_y, 3);  // units-per-inch
}

IppError add_urf_values(IppWriter& writer, const char* encoded) noexcept {
    if (encoded == nullptr || encoded[0] == '\0') return IppError::InvalidArgument;
    const char* cursor = encoded;
    bool first = true;
    while (*cursor != '\0') {
        const char* end = std::strchr(cursor, ',');
        const size_t length = end == nullptr ? std::strlen(cursor)
                                             : static_cast<size_t>(end - cursor);
        if (length == 0) return IppError::InvalidArgument;
        IppError error = IppError::Ok;
        if (first) {
            error = writer.add_string(IppValueTag::Keyword, "urf-supported", cursor, length);
            first = false;
        } else {
            error = writer.add_continuation(
                IppValueTag::Keyword, reinterpret_cast<const uint8_t*>(cursor), length);
        }
        if (error != IppError::Ok) return error;
        if (end == nullptr) break;
        cursor = end + 1;
    }
    return IppError::Ok;
}

IppError add_media_default(IppWriter& writer, const char* media) noexcept {
    if (media == nullptr || media[0] == '\0' ||
        writer.begin_collection("media-col-default") != IppError::Ok ||
        writer.add_collection_member_name("media-keyword") != IppError::Ok ||
        writer.add_continuation(IppValueTag::Keyword,
                                reinterpret_cast<const uint8_t*>(media),
                                std::strlen(media)) != IppError::Ok) {
        return IppError::InvalidArgument;
    }
    return writer.end_collection();
}

IppError add_printer_attributes(IppWriter& writer, const IppRequest& request,
                                const AirPrintCapabilities& capabilities) noexcept {
    const int32_t query_operations[] = {
        static_cast<int32_t>(IppOperation::CancelJob),
        static_cast<int32_t>(IppOperation::GetJobAttributes),
        static_cast<int32_t>(IppOperation::GetJobs),
        static_cast<int32_t>(IppOperation::GetPrinterAttributes),
    };
    const int32_t print_operations[] = {
        static_cast<int32_t>(IppOperation::PrintJob),
        static_cast<int32_t>(IppOperation::ValidateJob),
        static_cast<int32_t>(IppOperation::CancelJob),
        static_cast<int32_t>(IppOperation::GetJobAttributes),
        static_cast<int32_t>(IppOperation::GetJobs),
        static_cast<int32_t>(IppOperation::GetPrinterAttributes),
    };
    const char* const ipp_versions[] = {"1.1", "2.0"};
    const char* const compression[] = {"none"};
    const char* const sides[] = {"one-sided"};
    const char* const colors[] = {"monochrome", "color"};
    const char* const printer_reasons[] = {"none"};
    const char* const media[] = {capabilities.media_keyword};

    IppError error = IppError::Ok;
    if (requested_attribute(request, "operations-supported")) {
        const int32_t* operations = capabilities.accepting_jobs ? print_operations
                                                                  : query_operations;
        const size_t operation_count = capabilities.accepting_jobs
                                            ? sizeof(print_operations) / sizeof(print_operations[0])
                                            : sizeof(query_operations) / sizeof(query_operations[0]);
        error = add_enum_values(writer, "operations-supported", operations, operation_count);
    }
    if (error == IppError::Ok && requested_attribute(request, "ipp-versions-supported")) {
        error = add_keyword_values(writer, "ipp-versions-supported", ipp_versions,
                                   sizeof(ipp_versions) / sizeof(ipp_versions[0]));
    }
    if (error == IppError::Ok && requested_attribute(request, "charset-configured")) {
        error = writer.add_cstr(IppValueTag::Charset, "charset-configured", "utf-8");
    }
    if (error == IppError::Ok && requested_attribute(request, "charset-supported")) {
        error = writer.add_cstr(IppValueTag::Charset, "charset-supported", "utf-8");
    }
    if (error == IppError::Ok && requested_attribute(request, "natural-language-configured")) {
        error = writer.add_cstr(IppValueTag::NaturalLanguage,
                                "natural-language-configured", "en");
    }
    if (error == IppError::Ok &&
        requested_attribute(request, "generated-natural-language-supported")) {
        error = writer.add_cstr(IppValueTag::NaturalLanguage,
                                "generated-natural-language-supported", "en");
    }
    if (error == IppError::Ok && requested_attribute(request, "printer-uri-supported")) {
        error = writer.add_cstr(IppValueTag::Uri, "printer-uri-supported",
                                capabilities.printer_uri);
    }
    if (error == IppError::Ok && requested_attribute(request, "uri-authentication-supported")) {
        error = writer.add_cstr(IppValueTag::Keyword, "uri-authentication-supported", "none");
    }
    if (error == IppError::Ok && requested_attribute(request, "uri-security-supported")) {
        error = writer.add_cstr(IppValueTag::Keyword, "uri-security-supported", "none");
    }
    if (error == IppError::Ok && requested_attribute(request, "printer-name")) {
        error = writer.add_cstr(IppValueTag::NameWithoutLanguage, "printer-name",
                                capabilities.printer_name);
    }
    if (error == IppError::Ok && requested_attribute(request, "printer-make-and-model")) {
        error = writer.add_cstr(IppValueTag::TextWithoutLanguage,
                                "printer-make-and-model", capabilities.make_and_model);
    }
    if (error == IppError::Ok && requested_attribute(request, "printer-info")) {
        error = writer.add_cstr(IppValueTag::TextWithoutLanguage, "printer-info",
                                capabilities.printer_info);
    }
    if (error == IppError::Ok && requested_attribute(request, "printer-more-info") &&
        capabilities.printer_more_info != nullptr) {
        error = writer.add_cstr(IppValueTag::Uri, "printer-more-info",
                                capabilities.printer_more_info);
    }
    if (error == IppError::Ok && requested_attribute(request, "printer-location")) {
        error = writer.add_cstr(IppValueTag::TextWithoutLanguage, "printer-location",
                                capabilities.printer_location);
    }
    if (error == IppError::Ok && requested_attribute(request, "printer-state")) {
        error = writer.add_enum("printer-state", 3);  // idle
    }
    if (error == IppError::Ok && requested_attribute(request, "printer-state-reasons")) {
        error = add_keyword_values(writer, "printer-state-reasons", printer_reasons, 1);
    }
    if (error == IppError::Ok && requested_attribute(request, "printer-is-accepting-jobs")) {
        error = writer.add_boolean("printer-is-accepting-jobs", capabilities.accepting_jobs);
    }
    if (error == IppError::Ok && requested_attribute(request, "queued-job-count")) {
        error = writer.add_integer("queued-job-count",
                                   static_cast<int32_t>(capabilities.queued_job_count));
    }
    if (error == IppError::Ok && requested_attribute(request, "document-format-supported")) {
        error = writer.add_cstr(IppValueTag::MimeMediaType, "document-format-supported",
                                capabilities.document_format);
        if (error == IppError::Ok && std::strcmp(capabilities.document_format, "image/urf") == 0) {
            error = writer.add_continuation(
                IppValueTag::MimeMediaType,
                reinterpret_cast<const uint8_t*>("application/octet-stream"),
                std::strlen("application/octet-stream"));
        }
    }
    if (error == IppError::Ok && requested_attribute(request, "document-format-default")) {
        error = writer.add_cstr(IppValueTag::MimeMediaType, "document-format-default",
                                capabilities.document_format);
    }
    if (error == IppError::Ok && requested_attribute(request, "compression-supported")) {
        error = add_keyword_values(writer, "compression-supported", compression, 1);
    }
    if (error == IppError::Ok && requested_attribute(request, "media-supported")) {
        error = add_keyword_values(writer, "media-supported", media, 1);
    }
    if (error == IppError::Ok && requested_attribute(request, "media-col-default")) {
        error = add_media_default(writer, capabilities.media_keyword);
    }
    if (error == IppError::Ok && requested_attribute(request, "urf-supported")) {
        error = add_urf_values(writer, capabilities.urf_supported);
    }
    if (error == IppError::Ok && requested_attribute(request, "printer-resolution-supported")) {
        error = add_resolution_values(writer, "printer-resolution-supported",
                                       capabilities.dpi_x, capabilities.dpi_y);
    }
    if (error == IppError::Ok && requested_attribute(request, "printer-resolution-default")) {
        error = add_resolution_values(writer, "printer-resolution-default",
                                       capabilities.dpi_x, capabilities.dpi_y);
    }
    if (error == IppError::Ok && requested_attribute(request, "color-supported")) {
        error = writer.add_boolean("color-supported", capabilities.color_supported);
    }
    if (error == IppError::Ok && requested_attribute(request, "sides-supported")) {
        error = add_keyword_values(writer, "sides-supported", sides, 1);
    }
    if (error == IppError::Ok && requested_attribute(request, "print-color-mode-supported")) {
        if (capabilities.color_supported) {
            error = add_keyword_values(writer, "print-color-mode-supported", colors, 2);
        } else {
            error = writer.add_cstr(IppValueTag::Keyword,
                                    "print-color-mode-supported", "monochrome");
        }
    }
    if (error == IppError::Ok && requested_attribute(request, "multiple-document-jobs-supported")) {
        error = writer.add_boolean("multiple-document-jobs-supported", false);
    }
    if (error == IppError::Ok && requested_attribute(request, "printer-up-time")) {
        error = writer.add_integer("printer-up-time", 0);
    }
    return error;
}

}  // namespace

const char* airprint_response_error_name(AirPrintResponseError error) noexcept {
    switch (error) {
        case AirPrintResponseError::Ok: return "ok";
        case AirPrintResponseError::InvalidArgument: return "invalid_argument";
        case AirPrintResponseError::InvalidCapabilities: return "invalid_capabilities";
        case AirPrintResponseError::UnsupportedOperation: return "unsupported_operation";
        case AirPrintResponseError::OutputTooSmall: return "output_too_small";
    }
    return "unknown";
}

AirPrintResponseError validate_airprint_capabilities(
    const AirPrintCapabilities& capabilities) noexcept {
    if (!capabilities.enabled || capabilities.ipp_port == 0 ||
        capabilities.printer_uri == nullptr || capabilities.printer_name == nullptr ||
        capabilities.make_and_model == nullptr || capabilities.printer_info == nullptr ||
        capabilities.printer_location == nullptr || capabilities.media_keyword == nullptr ||
        capabilities.document_format == nullptr || capabilities.urf_supported == nullptr ||
        capabilities.dpi_x == 0 || capabilities.dpi_y == 0 ||
        capabilities.document_format[0] == '\0' || capabilities.urf_supported[0] == '\0') {
        return AirPrintResponseError::InvalidCapabilities;
    }
    if (std::strncmp(capabilities.printer_uri, "ipp://", 6) != 0) {
        return AirPrintResponseError::InvalidCapabilities;
    }
    if (std::strcmp(capabilities.document_format, "image/urf") != 0) {
        return AirPrintResponseError::InvalidCapabilities;
    }
    return AirPrintResponseError::Ok;
}

AirPrintResponseError build_get_printer_attributes_response(
    const IppRequest& request, const AirPrintCapabilities& capabilities,
    uint8_t* output, size_t output_capacity, size_t* output_length) noexcept {
    if (output_length == nullptr || output == nullptr) {
        return AirPrintResponseError::InvalidArgument;
    }
    *output_length = 0;
    if (request.error != IppRequestError::Ok ||
        request.operation != IppOperation::GetPrinterAttributes) {
        return AirPrintResponseError::UnsupportedOperation;
    }
    const AirPrintResponseError valid = validate_airprint_capabilities(capabilities);
    if (valid != AirPrintResponseError::Ok) return valid;

    IppWriter writer(output, output_capacity);
    if (writer.begin_response(request.header.version, IppStatus::SuccessfulOk,
                              request.header.request_id) != IppError::Ok ||
        writer.begin_group(IppGroupTag::OperationAttributes) != IppError::Ok ||
        writer.add_cstr(IppValueTag::Charset, "attributes-charset", "utf-8") !=
            IppError::Ok ||
        writer.add_cstr(IppValueTag::NaturalLanguage,
                        "attributes-natural-language", "en") != IppError::Ok ||
        writer.begin_group(IppGroupTag::PrinterAttributes) != IppError::Ok) {
        return AirPrintResponseError::OutputTooSmall;
    }
    const IppError attributes = add_printer_attributes(writer, request, capabilities);
    if (attributes != IppError::Ok || writer.end_attributes() != IppError::Ok) {
        return attributes == IppError::OutputTooSmall
                   ? AirPrintResponseError::OutputTooSmall
                   : AirPrintResponseError::InvalidArgument;
    }
    *output_length = writer.length();
    return AirPrintResponseError::Ok;
}

}  // namespace airprint
}  // namespace ijpd
