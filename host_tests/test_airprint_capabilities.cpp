// test_airprint_capabilities.cpp - Get-Printer-Attributes response tests.
#include "assert.h"

#include <cstdint>
#include <cstring>

#include "airprint_core/airprint_capabilities.h"
#include "airprint_core/ipp_attributes.h"
#include "airprint_core/ipp_codec.h"

using namespace ijpd::airprint;

namespace {

size_t make_request(uint8_t* buffer, size_t capacity, bool filter) {
    IppWriter writer(buffer, capacity);
    if (writer.begin_request({2, 0}, IppOperation::GetPrinterAttributes, 99) !=
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
    if (filter) {
        if (writer.add_cstr(IppValueTag::Keyword, "requested-attributes",
                            "printer-name") != IppError::Ok ||
            writer.add_continuation(IppValueTag::Keyword,
                                    reinterpret_cast<const uint8_t*>("printer-state"),
                                    std::strlen("printer-state")) != IppError::Ok) {
            return 0;
        }
    }
    if (writer.end_attributes() != IppError::Ok) return 0;
    return writer.length();
}

AirPrintCapabilities capabilities() {
    AirPrintCapabilities value;
    value.enabled = true;
    value.printer_uri = "ipp://printer.local/ipp/print";
    value.printer_more_info = "http://printer.local/";
    value.printer_name = "IJPD-Test";
    value.make_and_model = "IJPD AirPrint Test";
    value.printer_info = "Bounded AirPrint test printer";
    value.printer_location = "Lab";
    value.media_keyword = "iso_a4_210x297mm";
    value.document_format = "image/urf";
    value.urf_supported = "V1.4,CP1,RS600,W8,DM1,IS1,MT1,FN3";
    value.dpi_x = 203;
    value.dpi_y = 203;
    value.color_supported = false;
    value.duplex_supported = false;
    value.accepting_jobs = true;
    value.queued_job_count = 0;
    return value;
}

}  // namespace

int main() {
    uint8_t request_bytes[512] = {};
    const size_t request_length = make_request(request_bytes, sizeof(request_bytes), false);
    CHECK_TRUE(request_length != 0);

    IppRequest request{};
    const IppRequestPolicy policy{nullptr, true};
    CHECK_TRUE(parse_ipp_request(request_bytes, request_length, policy, &request) ==
               IppRequestError::Ok);

    uint8_t response_bytes[4096] = {};
    size_t response_length = 0;
    const AirPrintCapabilities caps = capabilities();
    CHECK_TRUE(validate_airprint_capabilities(caps) == AirPrintResponseError::Ok);
    CHECK_TRUE(build_get_printer_attributes_response(
                   request, caps, response_bytes, sizeof(response_bytes), &response_length) ==
               AirPrintResponseError::Ok);
    CHECK_TRUE(response_length > 8);

    IppReader response_reader;
    CHECK_TRUE(response_reader.begin(response_bytes, response_length) == IppError::Ok);
    CHECK_EQ_HEX(response_reader.header().code,
                 static_cast<uint16_t>(IppStatus::SuccessfulOk));
    CHECK_EQ(response_reader.header().request_id, 99u);
    CHECK_TRUE(response_reader.next().kind == IppReadKind::GroupStart);
    CHECK_TRUE(response_reader.next().kind == IppReadKind::Attribute);
    CHECK_TRUE(response_reader.next().kind == IppReadKind::Attribute);
    CHECK_TRUE(response_reader.next().kind == IppReadKind::GroupStart);
    IppReadResult attribute;
    size_t printer_attributes = 0;
    while ((attribute = response_reader.next()).error == IppError::Ok &&
           attribute.kind == IppReadKind::Attribute) {
        ++printer_attributes;
    }
    CHECK_TRUE(attribute.kind == IppReadKind::EndOfAttributes);
    CHECK_TRUE(printer_attributes > 10);
    CHECK_EQ(response_reader.document_length(), 0u);

    IppReader typed_reader;
    CHECK_TRUE(typed_reader.begin(response_bytes, response_length) == IppError::Ok);
    bool found_document_format = false;
    bool found_urf = false;
    for (;;) {
        const IppReadResult item = typed_reader.next();
        if (item.error != IppError::Ok || item.kind == IppReadKind::EndOfAttributes) break;
        if (item.kind != IppReadKind::Attribute) continue;
        if (attribute_name_equals(item.attribute, "document-format-supported")) {
            found_document_format = item.attribute.value_tag == IppValueTag::MimeMediaType;
        }
        if (attribute_name_equals(item.attribute, "urf-supported")) {
            found_urf = item.attribute.value_tag == IppValueTag::Keyword;
        }
    }
    CHECK_TRUE(found_document_format);
    CHECK_TRUE(found_urf);

    IppReader more_info_reader;
    CHECK_TRUE(more_info_reader.begin(response_bytes, response_length) == IppError::Ok);
    bool found_more_info = false;
    for (;;) {
        const IppReadResult item = more_info_reader.next();
        if (item.error != IppError::Ok || item.kind == IppReadKind::EndOfAttributes) break;
        if (item.kind == IppReadKind::Attribute &&
            attribute_name_equals(item.attribute, "printer-more-info")) {
            found_more_info = item.attribute.value_tag == IppValueTag::Uri;
        }
    }
    CHECK_TRUE(found_more_info);

    uint8_t filtered_request_bytes[512] = {};
    const size_t filtered_length =
        make_request(filtered_request_bytes, sizeof(filtered_request_bytes), true);
    IppRequest filtered_request{};
    CHECK_TRUE(parse_ipp_request(filtered_request_bytes, filtered_length, policy,
                                 &filtered_request) == IppRequestError::Ok);
    response_length = 0;
    CHECK_TRUE(build_get_printer_attributes_response(
                   filtered_request, caps, response_bytes, sizeof(response_bytes),
                   &response_length) == AirPrintResponseError::Ok);
    IppReader filtered_reader;
    CHECK_TRUE(filtered_reader.begin(response_bytes, response_length) == IppError::Ok);
    bool found_printer_name = false;
    bool found_printer_state = false;
    bool found_color = false;
    for (;;) {
        const IppReadResult item = filtered_reader.next();
        if (item.error != IppError::Ok || item.kind == IppReadKind::EndOfAttributes) break;
        if (item.kind != IppReadKind::Attribute) continue;
        found_printer_name = found_printer_name ||
            attribute_name_equals(item.attribute, "printer-name");
        found_printer_state = found_printer_state ||
            attribute_name_equals(item.attribute, "printer-state");
        found_color = found_color || attribute_name_equals(item.attribute, "color-supported");
    }
    CHECK_TRUE(found_printer_name);
    CHECK_TRUE(found_printer_state);
    CHECK_TRUE(!found_color);
    CHECK_TRUE(std::memcmp(response_bytes, "\x02\x00", 2) == 0);

    AirPrintCapabilities invalid = caps;
    invalid.printer_uri = "http://printer.local/ipp/print";
    CHECK_TRUE(validate_airprint_capabilities(invalid) ==
               AirPrintResponseError::InvalidCapabilities);
    CHECK_TRUE(build_get_printer_attributes_response(
                   request, invalid, response_bytes, sizeof(response_bytes), &response_length) ==
               AirPrintResponseError::InvalidCapabilities);

    uint8_t tiny[16] = {};
    response_length = 0;
    CHECK_TRUE(build_get_printer_attributes_response(
                   request, caps, tiny, sizeof(tiny), &response_length) ==
               AirPrintResponseError::OutputTooSmall);
    CHECK_EQ(response_length, 0u);

    return ::ijpd::test::summary("test_airprint_capabilities");
}
