// printer_app.cpp —— 组装与启动
#include "app/printer_app.h"

#include <cctype>
#include <cstdio>
#include <cstring>

#include "esp_mac.h"
#include "platform_esp/display_service.h"
#include "platform_esp/log_adapter.h"
#include "platform_esp/mdns_broadcast.h"
#include "platform_esp/task_layout.h"

namespace ijpd {
namespace {
constexpr const char* kTag = "app";
}

PrinterApp& PrinterApp::instance() noexcept {
    static PrinterApp app;
    return app;
}

void PrinterApp::set_config(const PrinterConfig* config) noexcept {
    if (config != nullptr) config_ = *config;
}

// ---------------------------------------------------------------- Router 钩子

Err PrinterApp::hook_device_info(void* user, JsonWriter& w) noexcept {
    auto* self = static_cast<PrinterApp*>(user);
    self->mem_.refresh();  // 实时 PSRAM 余量
    self->di_.psram_available = self->mem_.psram_available;
    self->di_.psram_sizes_valid = self->mem_.psram_available;
    self->di_.psram_total_bytes = self->mem_.psram_total_bytes;
    self->di_.psram_free_bytes = self->mem_.psram_free_bytes;
    return write_device_info_result(w, self->di_);
}

Err PrinterApp::hook_print_status(void*, JsonWriter& w) noexcept {
    return PrintService::hook_print_status(&PrintService::instance(), w);
}

Err PrinterApp::hook_start_print(void*, const StartPrintParams& p, JsonWriter& w) noexcept {
    return PrintService::hook_start_print(&PrintService::instance(), p, w);
}

Err PrinterApp::hook_cancel_print(void*, const CancelPrintParams& p, JsonWriter& w) noexcept {
    return PrintService::hook_cancel_print(&PrintService::instance(), p, w);
}

Err PrinterApp::hook_feed_paper(void*, const FeedPaperParams& p, JsonWriter& w) noexcept {
    return PrintService::hook_feed_paper(&PrintService::instance(), p, w);
}

Err PrinterApp::hook_cut_paper(void*, JsonWriter& w) noexcept {
    return PrintService::hook_cut_paper(&PrintService::instance(), w);
}

// ---------------------------------------------------------------- Host 适配

void PrinterApp::tcp_feed(void* user, int ch, const uint8_t* data, size_t len) noexcept {
    auto* self = static_cast<PrinterApp*>(user);
    self->plane_.feed(ch, data, len, task_layout::now_ms());
}

void PrinterApp::tcp_tick(void* user) noexcept {
    auto* self = static_cast<PrinterApp*>(user);
    self->tick_services_();
}

void PrinterApp::service_tick_task_(void* user) noexcept {
    auto* self = static_cast<PrinterApp*>(user);
    while (self->service_tick_running_) {
        self->tick_services_();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelete(nullptr);
}

void PrinterApp::tick_services_() noexcept {
    if (config_.services.ble) ble_.poll();  // BLE 队列 -> 控制面
    if (config_.services.mqtt) mqtt_.poll();
    plane_.tick(task_layout::now_ms());
    PrintService::instance().tick();  // 任务超时看护
    wifi_.stop_portal_if_connected();  // Wi-Fi 连上后关配网门户
    // mDNS 惰性启动:网络栈(netif)就绪后 mdns_init 才能成功,
    // 每 tick 重试直至成功(成功一次后不再进此分支)。
    if (!mdns_started_ && config_.services.ijpd_tcp && wifi_.connected()) {
        // device_id_ 形如 "IJPD-71D5D0":取 MAC 三字节十六进制转小写做主机名
        char lower[9] = {0};
        for (int i = 0; i < 6 && device_id_[5 + i]; ++i) {
            lower[i] = static_cast<char>(tolower(device_id_[5 + i]));
        }
        char hostname[24];
        snprintf(hostname, sizeof(hostname), "ijpd-%s", lower);
        if (mdns_broadcast_start(hostname, device_id_, config_.fw_version) ==
            Err::OK) {
            mdns_started_ = true;
        }
    }
}

void PrinterApp::tcp_channel_closed(void* user, int ch) noexcept {
    auto* self = static_cast<PrinterApp*>(user);
    self->plane_.detach(ch);
}

void PrinterApp::tcp_register_sink(void* user, int ch, void* sink_user,
                                   ControlTcpServer::SendFn send) noexcept {
    auto* self = static_cast<PrinterApp*>(user);
    ControlPlane::Sink sink{sink_user,
                            reinterpret_cast<ControlPlane::SendFn>(send)};
    self->plane_.set_sink(ch, sink);
}

void PrinterApp::ble_feed(void* user, const uint8_t* data, size_t len) noexcept {
    auto* self = static_cast<PrinterApp*>(user);
    self->plane_.feed(ControlPlane::kChannelBle, data, len, task_layout::now_ms());
}

Err PrinterApp::ble_send(void* user, const uint8_t* data, size_t len) noexcept {
    return static_cast<PrinterApp*>(user)->ble_.send(data, len);
}

void PrinterApp::mqtt_feed(void* user, const char* data, size_t len) noexcept {
    auto* self = static_cast<PrinterApp*>(user);
    self->plane_.feed(ControlPlane::kChannelMqtt,
                      reinterpret_cast<const uint8_t*>(data), len,
                      task_layout::now_ms());
}

Err PrinterApp::mqtt_send(void* user, const uint8_t* data, size_t len) noexcept {
    return static_cast<PrinterApp*>(user)->mqtt_.publish_event(
        reinterpret_cast<const char*>(data), len);
}

bool PrinterApp::data_accept(void* user, int fd, const char* peer) noexcept {
    (void)user;
    return PrintService::data_accept(&PrintService::instance(), fd, peer);
}

bool PrinterApp::raw_accept(void* user, int fd, const char* peer) noexcept {
    return PrintService::raw_accept(&PrintService::instance(), fd, peer);
}

// ---------------------------------------------------------------- 启动

Err PrinterApp::start() noexcept {
    // 1) PSRAM 内存（失败不致命：打印能力保持关闭，控制通道照常工作）
    const Err me = mem_.init();
    if (me != Err::OK) {
        IJPD_LOGE(kTag, "PSRAM init failed (%s) - printing disabled", err_name(me));
    }

    // 2) 设备标识（efuse MAC）
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(device_id_, sizeof(device_id_), "IJPD-%02X%02X%02X", mac[3], mac[4],
             mac[5]);
    // mDNS 广播推迟到 Wi-Fi 就绪后(tcp_tick 惰性启动,见 tcp_tick)

    if (config_.services.escpos_raw_tcp &&
        (config_.profile == nullptr ||
         !has_mode(config_.profile->data_modes, DataMode::EscPos))) {
        IJPD_LOGW(kTag, "escpos raw disabled: profile does not support escpos");
        config_.services.escpos_raw_tcp = false;
    }

    di_ = DeviceInfo{};
    di_.device_id = device_id_;
    di_.model = config_.device_model;
    di_.fw_version = config_.fw_version;
    di_.hal_mode = "mock";
    di_.profile = config_.profile;  // 中间件 Profile
    di_.psram_available = mem_.psram_available;
    di_.psram_sizes_valid = mem_.psram_available;
    di_.cap_paper_feed = true;   // Mock 走纸（真实电机 Phase 7）
    di_.cap_cutter = (config_.profile != nullptr) && config_.profile->has_cutter;
    di_.cap_mock_print = true;
    di_.services = config_.services;
    di_.escpos_raw_tcp_port = config_.services.escpos_raw_tcp
                                  ? config_.escpos_raw_tcp_port
                                  : 0;
    di_.airprint_port = config_.services.airprint ? config_.airprint_port : 0;

    // 3) 打印编排服务（Mock HAL）
    const Err de = DisplayService::instance().start();
    if (de != Err::OK) {
        IJPD_LOGW(kTag, "display start failed (%s) - 打印不受影响", err_name(de));
    }
    PrintService& svc = PrintService::instance();
    svc.set_mqtt_context(config_.mqtt_base_topic, device_id_);
    const Err se = svc.start(&mem_, &plane_);
    if (se != Err::OK) {
        IJPD_LOGE(kTag, "print service start failed: %s", err_name(se));
        // 打印能力降级，控制通道继续
    }

    // 4) 路由能力（打印=PSRAM 可用；走纸=Mock；切纸=Profile）
    const bool print_ok = (se == Err::OK) && mem_.psram_available;
    const bool cutter_ok = (config_.profile != nullptr) && config_.profile->has_cutter;
    router_.set_capabilities(Capabilities{print_ok, se == Err::OK, cutter_ok});
    router_.set_profile(config_.profile);
    CommandRouter::Hooks hooks;
    hooks.user = this;
    hooks.get_device_info = hook_device_info;
    hooks.get_print_status = hook_print_status;
    hooks.start_print = hook_start_print;
    hooks.cancel_print = hook_cancel_print;
    hooks.feed_paper = hook_feed_paper;
    hooks.cut_paper = hook_cut_paper;
    router_.set_hooks(hooks);

    // 4) 控制面
    const Err pe = plane_.init(&router_);
    if (pe != Err::OK) return pe;

    // 5) BLE sink（通道 2）
    plane_.set_sink(ControlPlane::kChannelBle, {this, ble_send});

    // 6) Wi-Fi / 网络栈（必须先于 TCP 服务器：esp_netif_init 在此触发）
    const bool network_needed = config_.services.wifi || config_.services.ijpd_tcp ||
                                config_.services.escpos_raw_tcp || config_.services.mqtt ||
                                config_.services.airprint;
    Err e = network_needed ? wifi_.start() : Err::OK;
    if (e != Err::OK) {
        IJPD_LOGW(kTag, "wifi start failed (%s) - BLE channel still available",
                  err_name(e));
    }

    // 7) 控制服务器（注册 TCP sink 0/1 + 空闲钩子）
    ControlTcpServer::Host tcp_host;
    tcp_host.user = this;
    tcp_host.feed = tcp_feed;
    tcp_host.tick = tcp_tick;
    tcp_host.on_channel_closed = tcp_channel_closed;
    tcp_host.register_sink = tcp_register_sink;
    if (config_.services.ijpd_tcp) {
        e = ctl_tcp_.start(cfg::CONTROL_TCP_PORT, tcp_host);
        if (e != Err::OK) {
            IJPD_LOGE(kTag, "control tcp start failed: %s", err_name(e));
            return e;
        }
    }

    if (config_.services.escpos_raw_tcp) {
        e = raw_tcp_.start(config_.escpos_raw_tcp_port, raw_accept, this);
        if (e != Err::OK) {
            IJPD_LOGE(kTag, "raw tcp start failed: %s", err_name(e));
            return e;
        }
    }

    // 8) 数据服务器（策略委托打印服务：仅任务等待握手时接受）
    if (config_.services.ijpd_tcp) {
        e = data_tcp_.start(cfg::DATA_TCP_PORT, data_accept, this);
        if (e != Err::OK) {
            IJPD_LOGE(kTag, "data tcp start failed: %s", err_name(e));
            return e;
        }
    }

    // 9) BLE
    BleControlServer::Host ble_host;
    ble_host.user = this;
    ble_host.feed = ble_feed;
    ble_host.send = ble_send;
    if (config_.services.ble) {
        e = ble_.start(config_.ble_name, ble_host);
        if (e != Err::OK) {
            IJPD_LOGE(kTag, "ble start failed: %s", err_name(e));
            return e;
        }
    }

    if (config_.services.mqtt) {
        MqttAdapter::Host mqtt_host;
        mqtt_host.user = this;
        mqtt_host.on_command = mqtt_feed;
        mqtt_host.on_chunk = PrintService::mqtt_chunk;
        mqtt_host.on_disconnected = PrintService::mqtt_disconnected;
        e = mqtt_.start(config_.mqtt_uri, config_.mqtt_base_topic, device_id_, mqtt_host);
        if (e != Err::OK) {
            IJPD_LOGE(kTag, "mqtt start failed: %s", err_name(e));
            return e;
        }
        plane_.set_sink(ControlPlane::kChannelMqtt, {this, mqtt_send});
    }

    if (config_.services.airprint) {
        IppServer::Host ipp_host;
        ipp_host.user = this;
        ipp_host.begin_page = PrintService::ipp_begin_page;
        ipp_host.write_row = PrintService::ipp_write_row;
        ipp_host.end_page = PrintService::ipp_end_page;
        e = ipp_.start(config_.airprint_port, config_.device_model, config_.profile,
                       ipp_host);
        if (e != Err::OK) {
            IJPD_LOGE(kTag, "IPP foundation start failed: %s", err_name(e));
            return e;
        }
    }

    if (!config_.services.ijpd_tcp &&
        (config_.services.ble || config_.services.wifi ||
         config_.services.escpos_raw_tcp || config_.services.mqtt)) {
        service_tick_running_ = true;
        e = task_layout::spawn("app_tick", service_tick_task_, this, 0,
                               task_layout::kPrioCtrl, 3072);
        if (e != Err::OK) {
            service_tick_running_ = false;
            return e;
        }
    }

    IJPD_LOGI(kTag, "started: device=%s fw=%s psram=%s", device_id_, config_.fw_version,
              mem_.psram_available ? "ok" : "unavailable");
    return Err::OK;
}

}  // namespace ijpd
