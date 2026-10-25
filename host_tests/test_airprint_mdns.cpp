// test_airprint_mdns.cpp - AirPrint Bonjour record and capability consistency tests.
#include "assert.h"

#include <cstring>

#include "airprint_core/airprint_mdns.h"

using namespace ijpd::airprint;

namespace {

AirPrintCapabilities capabilities() {
    AirPrintCapabilities value;
    value.enabled = true;
    value.printer_uri = "ipp://printer.local/ipp/print";
    value.printer_name = "IJPD-Test";
    value.make_and_model = "IJPD AirPrint Test";
    value.printer_info = "Bounded AirPrint test printer";
    value.printer_location = "Lab";
    value.media_keyword = "iso_a4_210x297mm";
    value.document_format = "image/urf";
    value.urf_supported = "V1.4,CP1,RS600,W8,DM1,IS1,MT1,FN3";
    value.dpi_x = 203;
    value.dpi_y = 203;
    return value;
}

const AirPrintTxtItem* find_txt(const AirPrintMdnsRecord& record, const char* key) {
    for (size_t i = 0; i < record.txt_count; ++i) {
        if (std::strcmp(record.txt[i].key, key) == 0) return &record.txt[i];
    }
    return nullptr;
}

}  // namespace

int main() {
    AirPrintMdnsRecord record{};
    const AirPrintCapabilities caps = capabilities();
    CHECK_TRUE(build_airprint_mdns_record(caps, "IJPD-71D5D0", "ijpd-71d5d0.local",
                                           &record) == AirPrintMdnsError::Ok);
    CHECK_EQ(record.port, 631);
    CHECK_EQ_STR(record.instance_name, "IJPD-Test");
    CHECK_EQ_STR(record.hostname, "ijpd-71d5d0.local");
    CHECK_EQ(std::strlen(record.uuid), 36u);
    CHECK_TRUE(record.uuid[14] == '5');
    CHECK_TRUE(record.uuid[19] == '8' || record.uuid[19] == '9' ||
               record.uuid[19] == 'a' || record.uuid[19] == 'b');
    CHECK_TRUE(record.txt_count >= 12u);
    CHECK_EQ_STR(find_txt(record, "rp")->value, "ipp/print");
    CHECK_EQ_STR(find_txt(record, "pdl")->value, "image/urf,application/octet-stream");
    CHECK_EQ_STR(find_txt(record, "URF")->value, caps.urf_supported);
    CHECK_EQ_STR(find_txt(record, "Color")->value, "F");
    CHECK_EQ_STR(find_txt(record, "Duplex")->value, "F");
    CHECK_EQ_STR(find_txt(record, "Scan")->value, "F");

    AirPrintMdnsRecord repeat{};
    CHECK_TRUE(build_airprint_mdns_record(caps, "IJPD-71D5D0", "ijpd-71d5d0.local",
                                           &repeat) == AirPrintMdnsError::Ok);
    CHECK_EQ_STR(record.uuid, repeat.uuid);

    AirPrintCapabilities invalid = caps;
    invalid.printer_uri = "http://printer.local/ipp/print";
    CHECK_TRUE(build_airprint_mdns_record(invalid, "IJPD-71D5D0", "ijpd-71d5d0.local",
                                           &record) == AirPrintMdnsError::InvalidCapabilities);
    invalid = caps;
    invalid.urf_supported = nullptr;
    CHECK_TRUE(build_airprint_mdns_record(invalid, "IJPD-71D5D0", "ijpd-71d5d0.local",
                                           &record) == AirPrintMdnsError::InvalidCapabilities);
    CHECK_TRUE(build_airprint_mdns_record(caps, nullptr, "ijpd-71d5d0.local", &record) ==
               AirPrintMdnsError::InvalidArgument);

    return ::ijpd::test::summary("test_airprint_mdns");
}
