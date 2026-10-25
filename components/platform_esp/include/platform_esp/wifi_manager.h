// ============================================================================
// wifi_manager.h —— Wi-Fi STA 管理（Phase 5）
//
// 凭据来源：NVS 中已保存的配网凭据；无可用凭据时启动 SoftAP HTTP 门户。
// 断线自动重连（事件驱动，1s 退避）。不记录密码。
// ============================================================================
#ifndef IJPD_PLATFORM_WIFI_MANAGER_H
#define IJPD_PLATFORM_WIFI_MANAGER_H

#include <cstdint>

#include "core/errors.h"
#include "esp_event.h"
#include "esp_timer.h"

namespace ijpd {

class ProvisioningPortal;

class WifiManager {
public:
    Err start() noexcept;  // 有凭据→STA；无→SoftAP 配网门户
    Err request_provisioning() noexcept;  // 延迟到当前控制响应发送后切换
    void process_pending_requests() noexcept;
    bool connected() const noexcept { return connected_; }
    ~WifiManager();

    // STA 连接成功后调用（关闭配网门户）
    void stop_portal_if_connected() noexcept;

private:
    static void event_handler(void* arg, esp_event_base_t event_base,
                              int32_t event_id, void* event_data);
    volatile bool connected_ = false;
    esp_timer_handle_t retry_timer_ = nullptr;
    int backoff_ms_ = 1000;
    ProvisioningPortal* portal_ = nullptr;
    volatile bool portal_active_ = false;  // 门户模式：禁止 STA 重试
    volatile bool provisioning_requested_ = false;
};

}  // namespace ijpd

#endif  // IJPD_PLATFORM_WIFI_MANAGER_H
