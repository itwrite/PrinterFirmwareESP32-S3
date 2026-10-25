// wifi_manager.cpp —— Wi-Fi STA + SoftAP 配网门户
#include "platform_esp/wifi_manager.h"

#include <cstring>

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "platform_esp/log_adapter.h"
#include "platform_esp/provisioning_portal.h"

namespace ijpd {
namespace {
constexpr const char* kTag = "wifi";
constexpr int kMaxBackoffMs = 8000;
constexpr int kInitBackoffMs = 1000;
}  // namespace

WifiManager::~WifiManager() {
    if (retry_timer_ != nullptr) {
        esp_timer_stop(retry_timer_);
        esp_timer_delete(retry_timer_);
    }
}

void WifiManager::event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data) {
    auto* self = static_cast<WifiManager*>(arg);
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        // 门户模式(APSTA 闲置 STA)不自动连接;空配置连接只会刷错误
        if (!self->portal_active_) {
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        self->connected_ = false;
        // 门户模式下不重试（重试会断开 SoftAP 客户端）
        if (self->portal_active_) {
            IJPD_LOGI(kTag, "portal active, skip retry");
            return;
        }
        {
            auto* evt = static_cast<wifi_event_sta_disconnected_t*>(event_data);
            IJPD_LOGE(kTag, "disconnect reason=%d", evt->reason);
        }
        esp_timer_stop(self->retry_timer_);
        esp_timer_start_once(self->retry_timer_,
                             static_cast<uint64_t>(self->backoff_ms_) * 1000);
        self->backoff_ms_ = (self->backoff_ms_ * 2 > kMaxBackoffMs)
                                ? kMaxBackoffMs
                                : self->backoff_ms_ * 2;
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        self->connected_ = true;
        self->backoff_ms_ = kInitBackoffMs;
        auto* evt = static_cast<ip_event_got_ip_t*>(event_data);
        IJPD_LOGI(kTag, "got ip " IPSTR, IP2STR(&evt->ip_info.ip));
    }
}

Err WifiManager::start() noexcept {
    // NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return Err::INTERNAL_ERROR;

    // 网络栈（TCP 服务器依赖）
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    // 重连定时器
    esp_timer_create_args_t timer_args = {};
    timer_args.callback = [](void*) {
        IJPD_LOGI(kTag, "reconnecting...");
        esp_wifi_connect();
    };
    timer_args.arg = nullptr;
    timer_args.name = "wifi_retry";
    if (esp_timer_create(&timer_args, &retry_timer_) != ESP_OK) {
        return Err::INTERNAL_ERROR;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    // 打印机为持续流式接收设备:关闭调制解调器省电(默认 MIN_MODEM 会在
    // DTIM 间休眠,侦听间隔可达 ~300ms,造成 RTT 尖峰与 TCP 吞吐骤降)。
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiManager::event_handler, this, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &WifiManager::event_handler, this, nullptr));

    // ---- 配网逻辑：有凭据则直连，无则开 SoftAP 配网门户 ----
    if (ProvisioningPortal::has_saved_credentials()) {
        IJPD_LOGI(kTag, "has saved credentials, trying STA...");
        if (ProvisioningPortal::try_connect_saved()) {
            IJPD_LOGI(kTag, "STA connected from saved credentials");
            return Err::OK;
        }
        IJPD_LOGW(kTag, "saved credentials failed, starting portal");
        // 停止所有 STA 活动（防止重连定时器干扰 SoftAP）
        esp_timer_stop(retry_timer_);
        esp_wifi_disconnect();
        esp_wifi_stop();
    }

    // 无凭据或连接失败 → 开配网门户
    portal_active_ = true;  // 禁止 STA 重试（防止断开 SoftAP）
    esp_timer_stop(retry_timer_);  // 停止当前的重连定时器
    portal_ = new ProvisioningPortal();
    const Err pe = portal_->start();
    if (pe != Err::OK) {
        IJPD_LOGW(kTag, "portal start failed: %d - 打印 TCP 不受影响", (int)pe);
        portal_active_ = false;
        // 不 return:portal 失败不应阻止后续 TCP 服务器启动
    }

    IJPD_LOGI(kTag, "provisioning portal active (connect to IJPD-Setup-XXXX)");
    return Err::OK;  // 配网模式不算失败
}

Err WifiManager::request_provisioning() noexcept {
    if (portal_active_ || provisioning_requested_) return Err::OK;
    provisioning_requested_ = true;
    return Err::OK;
}

void WifiManager::process_pending_requests() noexcept {
    if (!provisioning_requested_) return;
    provisioning_requested_ = false;
    if (portal_active_) return;
    if (portal_ == nullptr) portal_ = new ProvisioningPortal();

    portal_active_ = true;
    connected_ = false;
    esp_timer_stop(retry_timer_);
    esp_wifi_disconnect();
    esp_wifi_stop();
    const Err result = portal_->start_on_demand();
    if (result != Err::OK) {
        portal_active_ = false;
        IJPD_LOGE(kTag, "on-demand provisioning start failed: %s", err_name(result));
        return;
    }
    IJPD_LOGI(kTag, "on-demand provisioning portal active");
}

void WifiManager::stop_portal_if_connected() noexcept {
    if (portal_ != nullptr && connected_) {
        portal_->stop();
        delete portal_;
        portal_ = nullptr;
        portal_active_ = false;  // 恢复 STA 重试
        IJPD_LOGI(kTag, "portal stopped (STA connected)");
    }
}

}  // namespace ijpd
