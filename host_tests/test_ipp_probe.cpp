// test_ipp_probe.cpp - Bounded IPP query session integration tests.
#include "assert.h"

#include <cstdint>

#include "airprint_core/airprint_capabilities.h"
#include "airprint_core/ipp_codec.h"
#include "airprint_core/ipp_probe.h"

using namespace ijpd::airprint;

namespace {

AirPrintCapabilities capabilities() {
    AirPrintCapabilities value;
    value.enabled = true;
    value.printer_uri = "ipp://printer.local/ipp/print";
    value.printer_name = "IJPD-Probe";
    value.make_and_model = "IJPD Query Probe";
    value.printer_info = "read-only query probe";
    value.printer_location = "Lab";
    value.media_keyword = "iso_a4_210x297mm";
    value.document_format = "image/urf";
    value.urf_supported = "V1.4,CP1,RS600,W8,DM1,IS1,MT1,FN3";
    value.dpi_x = 203;
    value.dpi_y = 203;
    return value;
}

size_t make_request(uint8_t* output, size_t capacity, IppOperation operation,
                    uint32_t request_id) {
    IppWriter writer(output, capacity);
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

bool response_has_status(const uint8_t* data, size_t length, IppStatus status,
                         uint32_t request_id) {
    IppReader reader;
    return reader.begin(data, length) == IppError::Ok &&
           reader.header().code == static_cast<uint16_t>(status) &&
           reader.header().request_id == request_id;
}

}  // namespace

int main() {
    uint8_t request[512] = {};
    uint8_t response[8192] = {};
    const size_t request_length = make_request(
        request, sizeof(request), IppOperation::GetPrinterAttributes, 0x12345678);
    CHECK_TRUE(request_length != 0);

    IppProbeSession session;
    CHECK_TRUE(session.configure(capabilities()) == IppProbeError::Ok);
    const size_t split = request_length / 2;
    CHECK_TRUE(session.feed(request, split).error == IppProbeError::Ok);
    CHECK_TRUE(session.feed(request + split, request_length - split).error ==
               IppProbeError::Ok);
    size_t response_length = 0;
    CHECK_TRUE(session.finish(response, sizeof(response), &response_length) ==
               IppProbeError::Ok);
    CHECK_TRUE(response_has_status(response, response_length, IppStatus::SuccessfulOk,
                                   0x12345678));
    CHECK_TRUE(session.request().error == IppRequestError::Ok);

    uint8_t unsupported_request[512] = {};
    const size_t unsupported_length = make_request(
        unsupported_request, sizeof(unsupported_request), IppOperation::GetJobs, 9);
    session.reset();
    CHECK_TRUE(session.feed(unsupported_request, unsupported_length).error ==
               IppProbeError::Ok);
    response_length = 0;
    CHECK_TRUE(session.finish(response, sizeof(response), &response_length) ==
               IppProbeError::Ok);
    CHECK_TRUE(response_has_status(response, response_length,
                                   IppStatus::ServerErrorOperationNotSupported, 9));

    session.reset();
    const uint8_t malformed[] = {2, 0, 0, 11, 0, 0, 0, 3};
    CHECK_TRUE(session.feed(malformed, sizeof(malformed)).error == IppProbeError::Ok);
    response_length = 0;
    CHECK_TRUE(session.finish(response, sizeof(response), &response_length) ==
               IppProbeError::Ok);
    CHECK_TRUE(response_has_status(response, response_length,
                                   IppStatus::ClientErrorBadRequest, 3));

    IppProbeSession limited;
    IppProbeLimits limits;
    limits.max_request_bytes = 8;
    CHECK_TRUE(limited.configure(capabilities(), limits) == IppProbeError::Ok);
    CHECK_TRUE(limited.feed(request, request_length).error ==
               IppProbeError::RequestTooLarge);
    response_length = 0;
    CHECK_TRUE(limited.finish(response, sizeof(response), &response_length) ==
               IppProbeError::InvalidState);

    IppProbeSession capture_only;
    limits.max_request_bytes = 8;
    limits.discard_document_overflow = true;
    CHECK_TRUE(capture_only.configure(capabilities(), limits) == IppProbeError::Ok);
    const uint8_t oversized[64] = {};
    CHECK_TRUE(capture_only.feed(oversized, sizeof(oversized)).error == IppProbeError::Ok);
    response_length = 0;
    CHECK_TRUE(capture_only.finish(response, sizeof(response), &response_length) ==
               IppProbeError::Ok);

    IppProbeSession bad_limits;
    limits.max_request_bytes = kMaxIppProbeRequestBytes + 1;
    CHECK_TRUE(bad_limits.configure(capabilities(), limits) == IppProbeError::InvalidArgument);

    return ::ijpd::test::summary("test_ipp_probe");
}
