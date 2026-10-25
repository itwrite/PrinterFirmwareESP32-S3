// mdns_broadcast.cpp —— mDNS 服务广播实现
#include "platform_esp/mdns_broadcast.h"

#include <cstring>

#include "esp_log.h"
#include "mdns.h"

#include "platform_esp/log_adapter.h"

namespace ijpd {
namespace {
constexpr const char* kTag = "mdns";
bool s_initialized = false;
bool s_ijpd_started = false;

Err ensure_mdns_(const char* hostname, const char* device_id) noexcept {
    if (s_initialized) return Err::OK;
    if (hostname == nullptr || device_id == nullptr) return Err::INVALID_ARGUMENT;
    if (mdns_init() != ESP_OK) {
        IJPD_LOGE(kTag, "mdns_init failed");
        return Err::INTERNAL_ERROR;
    }
    mdns_hostname_set(hostname);
    mdns_instance_name_set(device_id);
    s_initialized = true;
    return Err::OK;
}
}  // namespace

Err mdns_broadcast_start(const char* hostname, const char* device_id,
                         const char* fw_version) noexcept {
    if (s_ijpd_started) return Err::OK;
    const Err init = ensure_mdns_(hostname, device_id);
    if (init != Err::OK) return init;

    // TXT:设备标识 / 固件版本(App 展示与兼容判断可用)
    mdns_txt_item_t txt[2] = {};
    char id_key[] = "device_id";
    char fw_key[] = "fw";
    txt[0].key = id_key;
    txt[0].value = const_cast<char*>(device_id);
    txt[1].key = fw_key;
    txt[1].value = const_cast<char*>(fw_version ? fw_version : "");

    if (mdns_service_add(device_id, "_ijpd", "_tcp", 9100, txt, 2) != ESP_OK) {
        IJPD_LOGE(kTag, "mdns_service_add failed");
        return Err::INTERNAL_ERROR;
    }
    s_ijpd_started = true;
    IJPD_LOGI(kTag, "broadcasting _ijpd._tcp:9100 as %s", device_id);
    return Err::OK;
}

}  // namespace ijpd
