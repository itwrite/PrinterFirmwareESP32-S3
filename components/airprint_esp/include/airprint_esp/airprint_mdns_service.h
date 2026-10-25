// ============================================================================
// airprint_mdns_service.h - Opt-in ESP-IDF AirPrint DNS-SD registration.
// ============================================================================
#ifndef IJPD_AIRPRINT_ESP_MDNS_SERVICE_H
#define IJPD_AIRPRINT_ESP_MDNS_SERVICE_H

#include "airprint_core/airprint_mdns.h"
#include "core/errors.h"

namespace ijpd {
namespace airprint {

class AirPrintMdnsService {
public:
    Err start(const char* hostname, const char* device_id,
              const AirPrintMdnsRecord& record) noexcept;
    void stop() noexcept;

    bool started() const noexcept { return started_; }

private:
    bool started_ = false;
    char instance_name_[kAirPrintMaxInstanceName] = {};
};

}  // namespace airprint
}  // namespace ijpd

#endif  // IJPD_AIRPRINT_ESP_MDNS_SERVICE_H
