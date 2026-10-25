// test_ipp_operations.cpp - IPP operation dispatch and standard error responses.
#include "assert.h"

#include <cstdint>
#include <cstring>

#include "airprint_core/airprint_capabilities.h"
#include "airprint_core/ipp_codec.h"
#include "airprint_core/ipp_operations.h"

using namespace ijpd::airprint;

namespace {

AirPrintCapabilities capabilities() {
    AirPrintCapabilities value;
    value.enabled = true;
    value.printer_uri = "ipp://printer.local/ipp/print";
    value.printer_name = "IJPD-Test";
    value.make_and_model = "IJPD AirPrint Test";
    value.printer_info = "test";
    value.printer_location = "Lab";
    value.media_keyword = "iso_a4_210x297mm";
    value.document_format = "image/urf";
    value.urf_supported = "V1.4,CP1,RS600,W8,DM1,IS1,MT1,FN3";
    value.dpi_x = 203;
    value.dpi_y = 203;
    return value;
}

size_t make_request(uint8_t* buffer, size_t capacity, IppOperation operation,
                    uint32_t request_id) {
    IppWriter writer(buffer, capacity);
    if (writer.begin_request({2, 0}, operation, request_id) != IppError::Ok ||
        writer.begin_group(IppGroupTag::OperationAttributes) != IppError::Ok ||
        writer.add_cstr(IppValueTag::Charset, "attributes-charset", "utf-8") !=
            IppError::Ok ||
        writer.add_cstr(IppValueTag::NaturalLanguage,
                        "attributes-natural-language", "en") != IppError::Ok ||
        writer.add_cstr(IppValueTag::Uri, "printer-uri",
                        "ipp://printer.local/ipp/print") != IppError::Ok ||
        writer.end_attributes() != IppError::Ok) {
        return 0;
    }
    return writer.length();
}

bool parse_response(const uint8_t* data, size_t length, IppStatus status,
                    uint32_t request_id, bool expect_printer_group,
                    bool expect_unsupported_group) {
    IppReader reader;
    if (reader.begin(data, length) != IppError::Ok ||
        reader.header().code != static_cast<uint16_t>(status) ||
        reader.header().request_id != request_id) {
        return false;
    }
    IppReadResult item = reader.next();
    if (item.kind != IppReadKind::GroupStart ||
        item.group != IppGroupTag::OperationAttributes) return false;
    if (reader.next().kind != IppReadKind::Attribute ||
        reader.next().kind != IppReadKind::Attribute) return false;
    item = reader.next();
    if (expect_printer_group) {
        if (item.kind != IppReadKind::GroupStart ||
            item.group != IppGroupTag::PrinterAttributes) return false;
        do {
            item = reader.next();
        } while (item.error == IppError::Ok && item.kind == IppReadKind::Attribute);
    } else if (expect_unsupported_group) {
        if (item.kind != IppReadKind::GroupStart ||
            item.group != IppGroupTag::UnsupportedAttributes) return false;
        item = reader.next();
    }
    return item.kind == IppReadKind::EndOfAttributes && reader.document_length() == 0;
}

size_t make_job_query(uint8_t* buffer, size_t capacity, const char* job_uri,
                      bool use_job_id) {
    IppWriter writer(buffer, capacity);
    if (writer.begin_request({2, 0}, IppOperation::GetJobAttributes, 0xAABBCCDD) !=
            IppError::Ok ||
        writer.begin_group(IppGroupTag::OperationAttributes) != IppError::Ok ||
        writer.add_cstr(IppValueTag::Charset, "attributes-charset", "utf-8") !=
            IppError::Ok ||
        writer.add_cstr(IppValueTag::NaturalLanguage,
                        "attributes-natural-language", "en") != IppError::Ok) {
        return 0;
    }
    if (use_job_id) {
        if (writer.add_integer("job-id", 1) != IppError::Ok) return 0;
    } else if (writer.add_cstr(IppValueTag::Uri, "job-uri", job_uri) != IppError::Ok) {
        return 0;
    }
    return writer.end_attributes() == IppError::Ok ? writer.length() : 0;
}

size_t make_validate_request(uint8_t* buffer, size_t capacity) {
    IppWriter writer(buffer, capacity);
    if (writer.begin_request({2, 0}, IppOperation::ValidateJob, 0xAABBCCEF) !=
            IppError::Ok ||
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
                        "application/octet-stream") != IppError::Ok) {
        return 0;
    }
    return writer.end_attributes() == IppError::Ok ? writer.length() : 0;
}

bool response_has_job_state(const uint8_t* data, size_t length, int32_t expected_state) {
    IppReader reader;
    if (reader.begin(data, length) != IppError::Ok) return false;
    IppReadResult item = reader.next();
    if (item.kind != IppReadKind::GroupStart ||
        item.group != IppGroupTag::OperationAttributes) return false;
    while ((item = reader.next()).error == IppError::Ok &&
           item.kind == IppReadKind::Attribute) {
    }
    if (item.kind != IppReadKind::GroupStart || item.group != IppGroupTag::JobAttributes) {
        return false;
    }
    while ((item = reader.next()).error == IppError::Ok &&
           item.kind == IppReadKind::Attribute) {
        if (item.attribute.name_length == 9 &&
            std::memcmp(item.attribute.name, "job-state", 9) == 0 &&
            item.attribute.value_tag == IppValueTag::Enum &&
            item.attribute.value_length == 4) {
            const int32_t value = (static_cast<int32_t>(item.attribute.value[0]) << 24) |
                                  (static_cast<int32_t>(item.attribute.value[1]) << 16) |
                                  (static_cast<int32_t>(item.attribute.value[2]) << 8) |
                                  static_cast<int32_t>(item.attribute.value[3]);
            return value == expected_state;
        }
    }
    return false;
}

}  // namespace

int main() {
    uint8_t request_bytes[512] = {};
    uint8_t response_bytes[4096] = {};
    size_t request_length = make_request(request_bytes, sizeof(request_bytes),
                                         IppOperation::GetJobs, 0x11223344);
    CHECK_TRUE(request_length != 0);
    IppRequest request{};
    CHECK_TRUE(parse_ipp_request(request_bytes, request_length, {nullptr, true}, &request) ==
               IppRequestError::Ok);
    size_t response_length = 0;
    CHECK_TRUE(dispatch_ipp_request(request, capabilities(), response_bytes,
                                    sizeof(response_bytes), &response_length) ==
               IppDispatchError::Ok);
    CHECK_TRUE(parse_response(response_bytes, response_length,
                              IppStatus::ServerErrorOperationNotSupported,
                              0x11223344, false, true));

    request_length = make_request(request_bytes, sizeof(request_bytes),
                                  IppOperation::GetPrinterAttributes, 0x55667788);
    CHECK_TRUE(request_length != 0);
    CHECK_TRUE(parse_ipp_request(request_bytes, request_length, {nullptr, true}, &request) ==
               IppRequestError::Ok);
    response_length = 0;
    CHECK_TRUE(dispatch_ipp_request(request, capabilities(), response_bytes,
                                    sizeof(response_bytes), &response_length) ==
               IppDispatchError::Ok);
    CHECK_TRUE(parse_response(response_bytes, response_length, IppStatus::SuccessfulOk,
                              0x55667788, true, false));

    IppRequest invalid{};
    invalid.header.version = {2, 0};
    invalid.header.request_id = 7;
    invalid.error = IppRequestError::MissingAttribute;
    invalid.status = IppStatus::ClientErrorBadRequest;
    response_length = 0;
    CHECK_TRUE(dispatch_ipp_request(invalid, capabilities(), response_bytes,
                                    sizeof(response_bytes), &response_length) ==
               IppDispatchError::Ok);
    CHECK_TRUE(parse_response(response_bytes, response_length,
                              IppStatus::ClientErrorBadRequest, 7, false, true));

    uint8_t tiny[16] = {};
    response_length = 0;
    CHECK_TRUE(dispatch_ipp_request(request, capabilities(), tiny, sizeof(tiny),
                                     &response_length) == IppDispatchError::OutputTooSmall);
    CHECK_EQ(response_length, 0u);

    IppJobStore jobs;
    IppJobSnapshot job{};
    CHECK_TRUE(jobs.create("ipp://printer.local/ipp/print", "photo", "alice",
                           "image/urf", 10, &job) == IppJobError::Ok);
    CHECK_TRUE(jobs.start_processing(job.id, 11) == IppJobError::Ok);
    CHECK_TRUE(jobs.complete(job.id, 12, 1, 7) == IppJobError::Ok);

    uint8_t job_request_bytes[512] = {};
    size_t job_request_length = make_job_query(
        job_request_bytes, sizeof(job_request_bytes), job.job_uri, false);
    CHECK_TRUE(job_request_length != 0);
    CHECK_TRUE(parse_ipp_request(job_request_bytes, job_request_length,
                                 {nullptr, true, false}, &request) == IppRequestError::Ok);
    response_length = 0;
    CHECK_TRUE(dispatch_ipp_request_with_jobs(request, capabilities(), &jobs, 20,
                                              response_bytes, sizeof(response_bytes),
                                              &response_length) == IppDispatchError::Ok);
    CHECK_TRUE(response_has_job_state(response_bytes, response_length, 9));

    job_request_length = make_job_query(
        job_request_bytes, sizeof(job_request_bytes), job.job_uri, true);
    CHECK_TRUE(job_request_length != 0);
    CHECK_TRUE(parse_ipp_request(job_request_bytes, job_request_length,
                                 {nullptr, true, false}, &request) == IppRequestError::Ok);
    response_length = 0;
    CHECK_TRUE(dispatch_ipp_request_with_jobs(request, capabilities(), &jobs, 20,
                                              response_bytes, sizeof(response_bytes),
                                              &response_length) == IppDispatchError::Ok);
    CHECK_TRUE(response_has_job_state(response_bytes, response_length, 9));

    job_request_length = make_request(job_request_bytes, sizeof(job_request_bytes),
                                      IppOperation::GetJobs, 0xAABBCCDE);
    CHECK_TRUE(job_request_length != 0);
    CHECK_TRUE(parse_ipp_request(job_request_bytes, job_request_length,
                                 {nullptr, true, false}, &request) == IppRequestError::Ok);
    response_length = 0;
    CHECK_TRUE(dispatch_ipp_request_with_jobs(request, capabilities(), &jobs, 20,
                                              response_bytes, sizeof(response_bytes),
                                              &response_length) == IppDispatchError::Ok);
    CHECK_TRUE(response_has_job_state(response_bytes, response_length, 9));

    const size_t validate_length = make_validate_request(
        job_request_bytes, sizeof(job_request_bytes));
    CHECK_TRUE(validate_length != 0);
    CHECK_TRUE(parse_ipp_request(job_request_bytes, validate_length,
                                 {"image/urf", true, false, true}, &request) ==
               IppRequestError::Ok);
    response_length = 0;
    CHECK_TRUE(dispatch_ipp_request_with_jobs(request, capabilities(), &jobs, 20,
                                              response_bytes, sizeof(response_bytes),
                                              &response_length) == IppDispatchError::Ok);
    CHECK_TRUE(parse_response(response_bytes, response_length, IppStatus::SuccessfulOk,
                              0xAABBCCEF, false, false));

    return ::ijpd::test::summary("test_ipp_operations");
}
