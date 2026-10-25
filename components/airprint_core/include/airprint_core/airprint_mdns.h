// ============================================================================
// airprint_mdns.h - Deterministic AirPrint Bonjour record builder.
// The ESP transport publishes one canonical _ipp._tcp service instance.
// ============================================================================
#ifndef IJPD_AIRPRINT_CORE_AIRPRINT_MDNS_H
#define IJPD_AIRPRINT_CORE_AIRPRINT_MDNS_H

#include <cstddef>
#include <cstdint>

#include "airprint_core/airprint_capabilities.h"

namespace ijpd {
namespace airprint {

inline constexpr size_t kAirPrintMaxTxtItems = 16;
inline constexpr size_t kAirPrintMaxTxtKey = 16;
inline constexpr size_t kAirPrintMaxTxtValue = 128;
inline constexpr size_t kAirPrintMaxInstanceName = 64;
inline constexpr size_t kAirPrintMaxHostname = 64;
inline constexpr size_t kAirPrintUuidTextLength = 37;

struct AirPrintTxtItem {
    char key[kAirPrintMaxTxtKey] = {};
    char value[kAirPrintMaxTxtValue] = {};
};

struct AirPrintMdnsRecord {
    char instance_name[kAirPrintMaxInstanceName] = {};
    char hostname[kAirPrintMaxHostname] = {};
    char uuid[kAirPrintUuidTextLength] = {};
    uint16_t port = 631;
    AirPrintTxtItem txt[kAirPrintMaxTxtItems] = {};
    size_t txt_count = 0;
};

enum class AirPrintMdnsError : uint8_t {
    Ok = 0,
    InvalidArgument,
    InvalidCapabilities,
    OutputTooSmall,
};

const char* airprint_mdns_error_name(AirPrintMdnsError error) noexcept;

AirPrintMdnsError build_airprint_mdns_record(
    const AirPrintCapabilities& capabilities, const char* device_id,
    const char* hostname, AirPrintMdnsRecord* out) noexcept;

}  // namespace airprint
}  // namespace ijpd

#endif  // IJPD_AIRPRINT_CORE_AIRPRINT_MDNS_H
