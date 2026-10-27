// ============================================================================
// ble_control_server.h —— BLE GATT 控制服务（Phase 5，IDF 内置 NimBLE）
//
// 服务（协议 §2.2，UUID 为 V1 固定值）：
//   Service  5a8f0001-7c2b-4d6e-9a01-3f5e8d7c2b01
//   写特征   5a8f0002-... （Write + Write Without Response）
//   通知特征 5a8f0003-... （Notify，按 MTU-3 分片发送，接收端按 '#' 重组）
//
// 数据流：中心设备写入分片（NimBLE host 任务上下文）→ 入队（固定容量）→
// ctrl 任务 poll() 排空 → Host.feed（ControlPlane 通道 kChannelBle 拆帧）。
// 响应/事件：Host.send → 按 MTU-3 分片 notify。
//
// 线程安全性：write 回调（host 任务）只访问队列；send/poll 仅 ctrl 任务；
//             连接状态为 volatile 标量（单写多读）。
// 注意：本头文件仅在设备侧构建中使用（依赖 NimBLE 头）。
// ============================================================================
#ifndef IJPD_PLATFORM_BLE_CONTROL_SERVER_H
#define IJPD_PLATFORM_BLE_CONTROL_SERVER_H

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "core/config_limits.h"
#include "core/ble_data_protocol.h"
#include "core/errors.h"
#include "host/ble_gatt.h"
#include "host/ble_gap.h"
#include "nimble/nimble_port.h"

namespace ijpd {

class BleControlServer {
public:
    struct Host {
        void* user = nullptr;
        void (*feed)(void* user, const uint8_t* data, size_t len) = nullptr;  // ctrl 线程
        Err (*send)(void* user, const uint8_t* data, size_t len) = nullptr;   // ctrl 线程
        Err (*data_packet)(void* user, const uint8_t* data, size_t len) = nullptr;
    };

    Err start(const char* device_name, const Host& host) noexcept;
    void stop() noexcept;

    // ctrl 任务周期调用：排空写队列 -> host.feed
    void poll() noexcept;

    // 发送（notify 分片）；未连接/未订阅时丢弃并计数
    Err send(const uint8_t* data, size_t len) noexcept;
    Err sendDataHelloAck(uint32_t token, uint16_t packet_bytes) noexcept;
    Err sendDataPacketAck(uint16_t packet_index) noexcept;

    bool connected() const noexcept { return conn_handle_ >= 0; }
    uint32_t dropped_tx_count() const noexcept { return dropped_tx_.load(std::memory_order_relaxed); }
    uint32_t dropped_rx_count() const noexcept { return dropped_rx_.load(std::memory_order_relaxed); }

    BleControlServer() noexcept = default;
    ~BleControlServer();

    // 写分片消息（NimBLE 回调为类的私有静态成员，需在类外可见的类型）
    struct RxMsg {
        uint16_t len;
        uint8_t buf[cfg::BLE_MTU_TARGET - 3];
    };
    static_assert(sizeof(RxMsg) <= 512, "队列项过大");

private:
    // NimBLE C 回调（访问私有状态）
    static int gatt_write_cb_(uint16_t conn, uint16_t attr, ble_gatt_access_ctxt* ctxt,
                              void* arg) noexcept;
    static int gatt_data_write_cb_(uint16_t conn, uint16_t attr,
                                  ble_gatt_access_ctxt* ctxt, void* arg) noexcept;
    static int gatt_read_cb_(uint16_t conn, uint16_t attr, ble_gatt_access_ctxt* ctxt,
                             void* arg) noexcept;
    static int gap_event_cb_(struct ble_gap_event* event, void* arg) noexcept;
    static void on_sync_() noexcept;
    static void on_reset_(int reason) noexcept;
    static void host_task_(void* param) noexcept;
    static void start_advertising_() noexcept;
    Err send_on_handle_(uint16_t handle, const uint8_t* data, size_t len) noexcept;

    void* queue_ = nullptr;    // QueueHandle_t
    void* data_queue_ = nullptr; // QueueHandle_t
    void* tx_lock_ = nullptr;  // SemaphoreHandle_t:串行化跨任务分块 notify
    Host host_{};
    volatile int conn_handle_ = -1;
    volatile bool notify_enabled_ = false;
    volatile bool data_ack_enabled_ = false;
    volatile uint16_t mtu_ = cfg::BLE_MTU_TARGET;
    uint16_t chr_write_handle_ = 0;
    uint16_t chr_notify_handle_ = 0;
    uint16_t chr_data_write_handle_ = 0;
    uint16_t chr_data_ack_handle_ = 0;
    std::atomic<uint32_t> dropped_tx_{0};
    std::atomic<uint32_t> dropped_rx_{0};
};

}  // namespace ijpd

#endif  // IJPD_PLATFORM_BLE_CONTROL_SERVER_H
