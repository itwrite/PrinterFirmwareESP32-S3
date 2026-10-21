// ============================================================================
// provisioning_portal.h —— SoftAP Web 配网门户（Phase M2）
//
// 流程：
//   1. 设备启动时若有已保存凭据 → 直接 STA 连接
//   2. 无凭据或连接失败 → 自动切换到 SoftAP 模式：
//      - 热点名: IJPD-Setup-XXXX（MAC 后 4 位）
//      - 密码:   12345678
//      - 网关:   192.168.4.1
//   3. 手机连接 IJPD-Setup-XXXX → 浏览器打开 http://192.168.4.1
//   4. 页面显示 Wi-Fi 扫描列表 → 选择 → 输入密码 → 提交
//   5. 设备保存凭据到 NVS → 切换 STA 连接 → 成功后重启 BLE/TCP 服务
//   6. 失败 → 页面提示错误 → 可重试
// ============================================================================
#ifndef IJPD_PLATFORM_PROVISIONING_PORTAL_H
#define IJPD_PLATFORM_PROVISIONING_PORTAL_H

#include <cstddef>
#include <cstdint>

#include "core/errors.h"

namespace ijpd {

class ProvisioningPortal {
public:
    // 启动配网门户（SoftAP + HTTP 服务器）
    // 返回 OK = 进入配网模式；否则 = 错误
    Err start() noexcept;

    // 停止配网门户（连接成功后调用）
    void stop() noexcept;

    // 检查是否有已保存的凭据
    static bool has_saved_credentials() noexcept;

    // 用已保存的凭据尝试 STA 连接
    // 返回 true = 连接成功
    static bool try_connect_saved() noexcept;

    // 获取当前状态
    bool is_active() const noexcept { return active_; }

    ProvisioningPortal() noexcept = default;
    ~ProvisioningPortal() { stop(); }

private:
    static constexpr const char* kNvsNamespace = "ijpd_wifi";
    static constexpr const char* kNvsKeySsid = "ssid";
    static constexpr const char* kNvsKeyPass = "pass";

    bool active_ = false;
    int server_fd_ = -1;
    void* portal_task_ = nullptr;
    volatile bool running_ = false;

    void portal_loop_() noexcept;
    void handle_client_(int fd) noexcept;
    void send_html_(int fd) noexcept;
    void send_scan_json_(int fd) noexcept;
    void handle_connect_(int fd, const char* body, size_t body_len) noexcept;
    void send_redirect_(int fd, const char* path) noexcept;
    void send_json_(int fd, const char* json) noexcept;

    static void task_entry_(void* arg) noexcept;
};

}  // namespace ijpd

#endif  // IJPD_PLATFORM_PROVISIONING_PORTAL_H
