// airprint_mdns_service.cpp - Opt-in ESP-IDF AirPrint DNS-SD registration.
#include "airprint_esp/airprint_mdns_service.h"

#include <cstring>

#include "esp_err.h"
#include "mdns.h"

namespace ijpd {
namespace airprint {
namespace {

Err map_mdns_error(esp_err_t error) noexcept {
    return error == ESP_ERR_NO_MEM ? Err::OUT_OF_MEMORY : Err::INTERNAL_ERROR;
}

}  // namespace

Err AirPrintMdnsService::start(const char* hostname, const char* device_id,
                               const AirPrintMdnsRecord& record) noexcept {
    if (started_ || hostname == nullptr || device_id == nullptr ||
        hostname[0] == '\0' || device_id[0] == '\0' || record.port == 0 ||
        record.instance_name[0] == '\0' || record.txt_count == 0 ||
        record.txt_count > kAirPrintMaxTxtItems) {
        return Err::INVALID_ARGUMENT;
    }
    std::memcpy(instance_name_, record.instance_name, sizeof(instance_name_));
    instance_name_[sizeof(instance_name_) - 1] = '\0';

    const esp_err_t init = mdns_init();
    if (init != ESP_OK && init != ESP_ERR_INVALID_STATE) return map_mdns_error(init);
    if (mdns_hostname_set(hostname) != ESP_OK || mdns_instance_name_set(device_id) != ESP_OK) {
        return Err::INTERNAL_ERROR;
    }

    mdns_txt_item_t txt[kAirPrintMaxTxtItems] = {};
    for (size_t i = 0; i < record.txt_count; ++i) {
        txt[i].key = const_cast<char*>(record.txt[i].key);
        txt[i].value = const_cast<char*>(record.txt[i].value);
    }
    const esp_err_t add = mdns_service_add(instance_name_, "_ipp", "_tcp", record.port,
                                           txt, record.txt_count);
    if (add != ESP_OK) return map_mdns_error(add);

    // iOS 16+ uses this subtype for AirPrint discovery. It points to the same
    // _ipp._tcp instance and must not be registered as a second service.
    const esp_err_t subtype = mdns_service_subtype_add_for_host(
        instance_name_, "_ipp", "_tcp", nullptr, "_universal");
    if (subtype != ESP_OK) {
        (void)mdns_service_remove_for_host(instance_name_, "_ipp", "_tcp", nullptr);
        return map_mdns_error(subtype);
    }

    started_ = true;
    return Err::OK;
}

void AirPrintMdnsService::stop() noexcept {
    if (!started_) return;
    (void)mdns_service_subtype_remove_for_host(instance_name_, "_ipp", "_tcp", nullptr,
                                               "_universal");
    (void)mdns_service_remove_for_host(instance_name_, "_ipp", "_tcp", nullptr);
    started_ = false;
    instance_name_[0] = '\0';
}

}  // namespace airprint
}  // namespace ijpd
