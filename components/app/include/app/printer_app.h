// ============================================================================
// printer_app.h —— 应用组装（Phase 5）
//
// 职责：初始化 PSRAM 内存 → 配置路由能力与钩子 → 汇入控制面（ControlPlane）
// → 启动控制 TCP / 数据 TCP / BLE / Wi-Fi。
//
// Phase 5 能力状态（诚实上报）：
//   start_print/print → UNSUPPORTED（打印流水线在 Phase 6 接入）
//   feed_paper / cut_paper → UNSUPPORTED（HAL 未接入）
//   get_device_info / get_print_status / cancel_print → 可用
// ============================================================================
#ifndef IJPD_APP_PRINTER_APP_H
#define IJPD_APP_PRINTER_APP_H

#include "app/control_plane.h"
#include "app/device_info.h"
#include "app/print_service.h"
#include "core/command_router.h"
#include "core/printer_config.h"
#include "platform_esp/ble_control_server.h"
#include "platform_esp/control_tcp_server.h"
#include "platform_esp/data_tcp_server.h"
#include "platform_esp/raw_tcp_server.h"
#include "platform_esp/mqtt_adapter.h"
#include "platform_esp/psram_allocator.h"
#include "platform_esp/wifi_manager.h"

namespace ijpd {

class PrinterApp {
public:
    static PrinterApp& instance() noexcept;  // 单例（app_main 静态持有）

    Err start() noexcept;

    // 兼容旧调用：只注入 Profile，其余服务保持默认关闭。
    void set_profile(const PrintProfile* profile) noexcept { config_.profile = profile; }
    // 注入完整打印机配置；未声明的服务默认不启动。
    void set_config(const PrinterConfig* config) noexcept;

private:
    PrinterApp() noexcept = default;
    PrinterConfig config_{};

    // Router 钩子（结果写入）
    static Err hook_device_info(void* user, JsonWriter& w) noexcept;
    static Err hook_print_status(void* user, JsonWriter& w) noexcept;
    static Err hook_start_print(void* user, const StartPrintParams& p, JsonWriter& w) noexcept;
    static Err hook_cancel_print(void* user, const CancelPrintParams& p, JsonWriter& w) noexcept;
    static Err hook_feed_paper(void* user, const FeedPaperParams& p, JsonWriter& w) noexcept;
    static Err hook_cut_paper(void* user, JsonWriter& w) noexcept;

    // ControlTcpServer Host 适配
    static void tcp_feed(void* user, int ch, const uint8_t* data, size_t len) noexcept;
    static void tcp_tick(void* user) noexcept;
    static void service_tick_task_(void* user) noexcept;
    void tick_services_() noexcept;
    static void tcp_channel_closed(void* user, int ch) noexcept;
    static void tcp_register_sink(void* user, int ch, void* sink_user,
                                  ControlTcpServer::SendFn send) noexcept;

    // BleControlServer Host 适配
    static void ble_feed(void* user, const uint8_t* data, size_t len) noexcept;
    static Err ble_send(void* user, const uint8_t* data, size_t len) noexcept;
    static void mqtt_feed(void* user, const char* data, size_t len) noexcept;
    static Err mqtt_send(void* user, const uint8_t* data, size_t len) noexcept;

    // DataTcpServer 策略：Phase 5 无任务等待，一律拒绝
    static bool data_accept(void* user, int fd, const char* peer) noexcept;
    static bool raw_accept(void* user, int fd, const char* peer) noexcept;

    CommandRouter router_;
    ControlPlane plane_;
    PlatformMemory mem_;
    WifiManager wifi_;
    ControlTcpServer ctl_tcp_;
    DataTcpServer data_tcp_;
    RawTcpServer raw_tcp_;
    MqttAdapter mqtt_;
    BleControlServer ble_;

    DeviceInfo di_{};
    char device_id_[16] = {0};
    bool mdns_started_ = false;  // Wi-Fi 就绪后惰性启动(tcp_tick 重试)
    volatile bool service_tick_running_ = false;
};

}  // namespace ijpd

#endif  // IJPD_APP_PRINTER_APP_H
