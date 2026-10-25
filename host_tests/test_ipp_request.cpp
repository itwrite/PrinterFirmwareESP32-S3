// test_ipp_request.cpp - IPP operation attribute and document validation tests.
#include "assert.h"

#include <cstdint>

#include "airprint_core/ipp_codec.h"
#include "airprint_core/ipp_request.h"

using namespace ijpd::airprint;

namespace {

size_t make_print_job(uint8_t* buffer, size_t capacity, bool add_document_format,
                      bool duplicate_charset, bool add_document,
                      const char* printer_uri = "ipp://printer.local/ipp/print",
                      const char* document_format = "image/urf") {
    IppWriter writer(buffer, capacity);
    if (writer.begin_request({2, 0}, IppOperation::PrintJob, 42) != IppError::Ok ||
        writer.begin_group(IppGroupTag::OperationAttributes) != IppError::Ok ||
        writer.add_cstr(IppValueTag::Charset, "attributes-charset", "utf-8") != IppError::Ok ||
        (duplicate_charset &&
         writer.add_cstr(IppValueTag::Charset, "attributes-charset", "utf-8") !=
             IppError::Ok) ||
        writer.add_cstr(IppValueTag::NaturalLanguage,
                        "attributes-natural-language", "en") != IppError::Ok ||
        writer.add_cstr(IppValueTag::Uri, "printer-uri", printer_uri) != IppError::Ok) {
        return 0;
    }
    if (add_document_format &&
        writer.add_cstr(IppValueTag::MimeMediaType, "document-format", document_format) !=
            IppError::Ok) {
        return 0;
    }
    if (writer.end_attributes() != IppError::Ok) return 0;
    if (add_document) {
        const uint8_t body[] = {0x55, 0xAA};
        if (writer.append_document(body, sizeof(body)) != IppError::Ok) return 0;
    }
    return writer.length();
}

size_t make_printer_attributes(uint8_t* buffer, size_t capacity, bool add_job_group,
                               bool add_document) {
    IppWriter writer(buffer, capacity);
    if (writer.begin_request({2, 0}, IppOperation::GetPrinterAttributes, 7) !=
            IppError::Ok ||
        writer.begin_group(IppGroupTag::OperationAttributes) != IppError::Ok ||
        writer.add_cstr(IppValueTag::Charset, "attributes-charset", "utf-8") !=
            IppError::Ok ||
        writer.add_cstr(IppValueTag::NaturalLanguage,
                        "attributes-natural-language", "en") != IppError::Ok ||
        writer.add_cstr(IppValueTag::Uri, "printer-uri",
                        "ipp://printer.local/ipp/print") != IppError::Ok) {
        return 0;
    }
    if (add_job_group && writer.begin_group(IppGroupTag::JobAttributes) != IppError::Ok) {
        return 0;
    }
    if (writer.end_attributes() != IppError::Ok) return 0;
    if (add_document) {
        const uint8_t body[] = {1};
        if (writer.append_document(body, sizeof(body)) != IppError::Ok) return 0;
    }
    return writer.length();
}

size_t make_job_query(uint8_t* buffer, size_t capacity, IppOperation operation,
                      bool add_job_id, bool add_job_uri) {
    IppWriter writer(buffer, capacity);
    if (writer.begin_request({2, 0}, operation, 8) != IppError::Ok ||
        writer.begin_group(IppGroupTag::OperationAttributes) != IppError::Ok ||
        writer.add_cstr(IppValueTag::Charset, "attributes-charset", "utf-8") !=
            IppError::Ok ||
        writer.add_cstr(IppValueTag::NaturalLanguage,
                        "attributes-natural-language", "en") != IppError::Ok) {
        return 0;
    }
    if (add_job_id && writer.add_integer("job-id", 1) != IppError::Ok) return 0;
    if (add_job_uri &&
        writer.add_cstr(IppValueTag::Uri, "job-uri",
                        "ipp://printer.local/ipp/print/job/1") != IppError::Ok) {
        return 0;
    }
    return writer.end_attributes() == IppError::Ok ? writer.length() : 0;
}

size_t make_duplicate_validate(uint8_t* buffer, size_t capacity) {
    IppWriter writer(buffer, capacity);
    if (writer.begin_request({2, 0}, IppOperation::ValidateJob, 12) != IppError::Ok ||
        writer.begin_group(IppGroupTag::OperationAttributes) != IppError::Ok ||
        writer.add_cstr(IppValueTag::Charset, "attributes-charset", "utf-8") !=
            IppError::Ok ||
        writer.add_cstr(IppValueTag::NaturalLanguage,
                        "attributes-natural-language", "en") != IppError::Ok ||
        writer.add_cstr(IppValueTag::Uri, "printer-uri",
                        "ipp://printer.local/ipp/print") != IppError::Ok ||
        writer.add_cstr(IppValueTag::MimeMediaType, "document-format", "image/urf") !=
            IppError::Ok ||
        writer.add_cstr(IppValueTag::MimeMediaType, "document-format",
                        "application/octet-stream") != IppError::Ok ||
        writer.end_attributes() != IppError::Ok) {
        return 0;
    }
    return writer.length();
}

size_t make_duplicate_user_job(uint8_t* buffer, size_t capacity) {
    IppWriter writer(buffer, capacity);
    if (writer.begin_request({2, 0}, IppOperation::PrintJob, 13) != IppError::Ok ||
        writer.begin_group(IppGroupTag::OperationAttributes) != IppError::Ok ||
        writer.add_cstr(IppValueTag::Charset, "attributes-charset", "utf-8") !=
            IppError::Ok ||
        writer.add_cstr(IppValueTag::NaturalLanguage,
                        "attributes-natural-language", "en") != IppError::Ok ||
        writer.add_cstr(IppValueTag::Uri, "printer-uri",
                        "ipp://printer.local/ipp/print") != IppError::Ok ||
        writer.add_cstr(IppValueTag::MimeMediaType, "document-format", "image/urf") !=
            IppError::Ok ||
        writer.add_cstr(IppValueTag::NameWithoutLanguage, "requesting-user-name", "alice") !=
            IppError::Ok ||
        writer.add_cstr(IppValueTag::NameWithoutLanguage, "requesting-user-name", "alice") !=
            IppError::Ok ||
        writer.end_attributes() != IppError::Ok) {
        return 0;
    }
    const uint8_t body[] = {0x55};
    return writer.append_document(body, sizeof(body)) == IppError::Ok ? writer.length() : 0;
}

}  // namespace

int main() {
    uint8_t buffer[512] = {};
    const IppRequestPolicy urf_policy{"image/urf", true};
    IppRequest request{};

    size_t length = make_print_job(buffer, sizeof(buffer), true, false, true);
    CHECK_TRUE(length != 0);
    CHECK_TRUE(parse_ipp_request(buffer, length, urf_policy, &request) ==
               IppRequestError::Ok);
    CHECK_TRUE(request.operation == IppOperation::PrintJob);
    CHECK_EQ(request.attribute_count, 4u);
    CHECK_EQ(request.document_length, 2u);
    CHECK_TRUE(request.status == IppStatus::SuccessfulOk);

    length = make_print_job(buffer, sizeof(buffer), false, false, true);
    CHECK_TRUE(parse_ipp_request(buffer, length, urf_policy, &request) ==
               IppRequestError::MissingAttribute);
    CHECK_TRUE(request.status == IppStatus::ClientErrorBadRequest);

    length = make_print_job(buffer, sizeof(buffer), true, false, false);
    CHECK_TRUE(parse_ipp_request(buffer, length, urf_policy, &request) ==
               IppRequestError::MissingDocument);
    CHECK_TRUE(request.status == IppStatus::ClientErrorDocumentFormatError);

    length = make_print_job(buffer, sizeof(buffer), true, true, true);
    CHECK_TRUE(parse_ipp_request(buffer, length, urf_policy, &request) ==
               IppRequestError::DuplicateAttribute);

    length = make_print_job(buffer, sizeof(buffer), true, false, true);
    IppRequestPolicy wrong_format{"image/jpeg", true};
    CHECK_TRUE(parse_ipp_request(buffer, length, wrong_format, &request) ==
               IppRequestError::UnsupportedDocumentFormat);
    CHECK_TRUE(request.status == IppStatus::ClientErrorDocumentFormatNotSupported);

    length = make_printer_attributes(buffer, sizeof(buffer), false, false);
    CHECK_TRUE(parse_ipp_request(buffer, length, urf_policy, &request) ==
               IppRequestError::Ok);
    CHECK_TRUE(request.operation == IppOperation::GetPrinterAttributes);
    CHECK_EQ(request.document_length, 0u);

    length = make_printer_attributes(buffer, sizeof(buffer), true, false);
    CHECK_TRUE(parse_ipp_request(buffer, length, urf_policy, &request) ==
               IppRequestError::InvalidGroupOrder);

    length = make_printer_attributes(buffer, sizeof(buffer), false, true);
    CHECK_TRUE(parse_ipp_request(buffer, length, urf_policy, &request) ==
               IppRequestError::UnexpectedDocument);

    uint8_t bad_uri[512] = {};
    length = make_print_job(bad_uri, sizeof(bad_uri), true, false, true,
                            "http://printer.local/ipp/print");
    CHECK_TRUE(length != 0);
    CHECK_TRUE(parse_ipp_request(bad_uri, length, urf_policy, &request) ==
               IppRequestError::InvalidAttributeValue);
    IppRequestPolicy permissive_uri{"image/urf", false};
    CHECK_TRUE(parse_ipp_request(bad_uri, length, permissive_uri, &request) ==
               IppRequestError::Ok);

    length = make_job_query(buffer, sizeof(buffer), IppOperation::GetJobAttributes,
                            false, true);
    CHECK_TRUE(length != 0);
    CHECK_TRUE(parse_ipp_request(buffer, length, urf_policy, &request) ==
               IppRequestError::Ok);
    CHECK_TRUE(request.find_first("printer-uri") == nullptr);
    CHECK_TRUE(request.find_first("job-uri") != nullptr);

    length = make_job_query(buffer, sizeof(buffer), IppOperation::CancelJob, true, false);
    CHECK_TRUE(length != 0);
    CHECK_TRUE(parse_ipp_request(buffer, length, urf_policy, &request) ==
               IppRequestError::Ok);

    length = make_job_query(buffer, sizeof(buffer), IppOperation::GetJobAttributes,
                            false, false);
    CHECK_TRUE(length != 0);
    CHECK_TRUE(parse_ipp_request(buffer, length, urf_policy, &request) ==
               IppRequestError::MissingAttribute);

    length = make_duplicate_validate(buffer, sizeof(buffer));
    CHECK_TRUE(length != 0);
    IppRequestPolicy duplicate_policy{"image/urf", true, false, true};
    CHECK_TRUE(parse_ipp_request(buffer, length, duplicate_policy, &request) ==
               IppRequestError::Ok);

    length = make_duplicate_user_job(buffer, sizeof(buffer));
    CHECK_TRUE(length != 0);
    IppRequestPolicy print_policy{"image/urf", true, true, true};
    CHECK_TRUE(parse_ipp_request(buffer, length, print_policy, &request) ==
               IppRequestError::Ok);

    length = make_print_job(buffer, sizeof(buffer), true, false, true,
                            "ipp://printer.local/ipp/print", "application/octet-stream");
    CHECK_TRUE(length != 0);
    CHECK_TRUE(parse_ipp_request(buffer, length, urf_policy, &request) ==
               IppRequestError::Ok);

    return ::ijpd::test::summary("test_ipp_request");
}
