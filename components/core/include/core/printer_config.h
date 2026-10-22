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

struct PrinterConfig {
    const PrintProfile* profile = nullptr;
    PrinterServices services{};
    const char* device_model = "IJPD-DEV";
    const char* fw_version = "0.1.0";
    const char* ble_name = "IJPD-Printer";
    uint16_t escpos_raw_tcp_port = 9102;
    uint16_t airprint_port = 631;
    const char* mqtt_uri = nullptr;
    const char* mqtt_base_topic = nullptr;
};

}  // namespace ijpd

#endif  // IJPD_CORE_PRINTER_CONFIG_H
