// ============================================================================
// printer_config.h - Capability-driven printer and service configuration.
//
// A firmware image can be reused across printer variants by injecting one
// static configuration. Services are opt-in: an omitted capability stays off.
// ============================================================================
#ifndef IJPD_CORE_PRINTER_CONFIG_H
#define IJPD_CORE_PRINTER_CONFIG_H

#include <cstdint>

#include "core/print_profile.h"

namespace ijpd {

struct PrinterServices {
    bool wifi = false;
    bool ble = false;
    bool ijpd_tcp = false;
    bool escpos_raw_tcp = false;
    bool mqtt = false;
    bool airprint = false;
};

// Explicit settings for the opt-in AirPrint service. These values are never
// inferred from a PrintProfile and are ignored by the default build.
struct AirPrintConfig {
    uint16_t port = 631;
    bool accept_print_jobs = false;
    const char* printer_more_info = nullptr;
    const char* printer_name = nullptr;
    const char* make_and_model = nullptr;
    const char* printer_info = nullptr;
    const char* printer_location = "";
    const char* media_keyword = nullptr;
    const char* document_format = "image/urf";
    const char* urf_supported = nullptr;
    uint16_t dpi_x = 0;
    uint16_t dpi_y = 0;
    bool color_supported = false;
    bool duplex_supported = false;
};

struct PrinterConfig {
    const PrintProfile* profile = nullptr;
    PrinterServices services{};
    const char* device_model = "IJPD-DEV";
    const char* fw_version = "0.1.0";
    const char* ble_name = "IJPD-Printer";
    uint16_t escpos_raw_tcp_port = 9102;
    const char* mqtt_uri = nullptr;
    const char* mqtt_base_topic = nullptr;
    AirPrintConfig airprint{};
};

}  // namespace ijpd

#endif  // IJPD_CORE_PRINTER_CONFIG_H
