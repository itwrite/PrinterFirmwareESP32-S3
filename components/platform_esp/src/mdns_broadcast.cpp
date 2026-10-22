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
bool s_ipp_started = false;

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

Err mdns_broadcast_ipp_start(const char* hostname, const char* device_id,
                             const char* model, uint16_t port,
                             const PrintProfile* profile) noexcept {
    if (s_ipp_started) return Err::OK;
    if (profile == nullptr || profile->pixel_format != PixelFormat::Mono1 ||
        !has_mode(profile->data_modes, DataMode::Raster)) {
        return Err::UNSUPPORTED;
    }
    const Err init = ensure_mdns_(hostname, device_id);
    if (init != Err::OK) return init;

    char k_txtvers[] = "txtvers";
    char v_txtvers[] = "1";
    char k_qtotal[] = "qtotal";
    char v_qtotal[] = "1";
    char k_ty[] = "ty";
    char k_product[] = "product";
    char k_note[] = "note";
    char k_admin[] = "adminurl";
    char k_priority[] = "priority";
    char k_uuid[] = "UUID";
    char k_rp[] = "rp";
    char k_pdl[] = "pdl";
    char k_color[] = "Color";
    char k_duplex[] = "Duplex";
    char k_scan[] = "Scan";
    char v_ty[64] = {};
    char v_product[96] = {};
    char v_admin[96] = {};
    char v_uuid[64] = {};
    snprintf(v_ty, sizeof(v_ty), "%s", model != nullptr ? model : device_id);
    snprintf(v_product, sizeof(v_product), "(IJPD/%s)", model != nullptr ? model : "Printer");
    snprintf(v_admin, sizeof(v_admin), "http://%s.local/", hostname);
    snprintf(v_uuid, sizeof(v_uuid), "uuid:00000000-0000-0000-0000-0000%s", device_id + 5);
    char v_priority[] = "0";
    char v_rp[] = "ipp/print";
    char v_pdl[] = "image/pwg-raster";
    char v_false[] = "F";
    mdns_txt_item_t txt[] = {
        {k_txtvers, v_txtvers}, {k_qtotal, v_qtotal}, {k_ty, v_ty},
        {k_product, v_product}, {k_note, v_ty}, {k_admin, v_admin},
        {k_priority, v_priority}, {k_uuid, v_uuid}, {k_rp, v_rp},
        {k_pdl, v_pdl}, {k_color, v_false}, {k_duplex, v_false},
        {k_scan, v_false},
    };
    if (mdns_service_add(device_id, "_ipp", "_tcp", port, txt,
                         sizeof(txt) / sizeof(txt[0])) != ESP_OK) {
        IJPD_LOGE(kTag, "mdns_service_add _ipp failed");
        return Err::INTERNAL_ERROR;
    }
    s_ipp_started = true;
    IJPD_LOGI(kTag, "broadcasting _ipp._tcp:%u as %s", port, device_id);
    return Err::OK;
}

}  // namespace ijpd
