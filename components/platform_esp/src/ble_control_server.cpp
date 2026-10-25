// ble_control_server.cpp —— NimBLE 外设（GATT 写/通知 + 分片重组）
#include "platform_esp/ble_control_server.h"

#include <atomic>
#include <cstring>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_hs_id.h"
#include "host/ble_hs_mbuf.h"
#include "host/util/util.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "platform_esp/log_adapter.h"

namespace ijpd {
namespace {

constexpr const char* kTag = "ble_ctl";

// UUID（NimBLE 要求小端字节序）：5a8f00XX-7c2b-4d6e-9a01-3f5e8d7c2b01
// 字符串 "5a8f0001-7c2b-4d6e-9a01-3f5e8d7c2b01" 按字节反转即下述数组
constexpr uint8_t kSvcUuidLe[16] = {0x01, 0x2b, 0x7c, 0x8d, 0x5e, 0x3f, 0x01, 0x9a,
                                    0x6e, 0x4d, 0x2b, 0x7c, 0x01, 0x00, 0x8f, 0x5a};
constexpr uint8_t kWriteUuidLe[16] = {0x02, 0x2b, 0x7c, 0x8d, 0x5e, 0x3f, 0x01, 0x9a,
                                      0x6e, 0x4d, 0x2b, 0x7c, 0x01, 0x00, 0x8f, 0x5a};
constexpr uint8_t kNotifyUuidLe[16] = {0x03, 0x2b, 0x7c, 0x8d, 0x5e, 0x3f, 0x01, 0x9a,
                                       0x6e, 0x4d, 0x2b, 0x7c, 0x01, 0x00, 0x8f, 0x5a};

BleControlServer* g_self = nullptr;  // NimBLE sync/reset 回调无上下文

ble_uuid128_t g_svc_uuid;
ble_uuid128_t g_write_uuid;
ble_uuid128_t g_notify_uuid;

}  // namespace

void BleControlServer::start_advertising_() noexcept {
    struct ble_hs_adv_fields fields;
    struct ble_hs_adv_fields rsp_fields;
    memset(&fields, 0, sizeof(fields));
    memset(&rsp_fields, 0, sizeof(rsp_fields));

    // Legacy advertising data is limited to 31 bytes. Keep the service UUID
    // in the primary packet and put the human-readable name in scan response:
    // flags (3) + UUID128 (18) + name (14) would otherwise exceed the limit.
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &g_svc_uuid;
    fields.uuids128_is_complete = 1;
    fields.num_uuids128 = 1;
    rsp_fields.name = reinterpret_cast<const uint8_t*>(ble_svc_gap_device_name());
    rsp_fields.name_len = static_cast<uint8_t>(strlen(ble_svc_gap_device_name()));
    rsp_fields.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        IJPD_LOGE(kTag, "adv fields rc=%d", rc);
        return;
    }
    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        IJPD_LOGE(kTag, "adv response fields rc=%d", rc);
        return;
    }
    struct ble_gap_adv_params params;
    memset(&params, 0, sizeof(params));
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, nullptr, BLE_HS_FOREVER, &params,
                           &BleControlServer::gap_event_cb_, nullptr);
    if (rc != 0) {
        IJPD_LOGE(kTag, "adv start rc=%d", rc);
    } else {
        IJPD_LOGI(kTag, "advertising as %s", ble_svc_gap_device_name());
    }
}

int BleControlServer::gap_event_cb_(struct ble_gap_event* event, void* arg) noexcept {
    (void)arg;
    BleControlServer* self = g_self;
    if (self == nullptr) return 0;
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                self->conn_handle_ = event->connect.conn_handle;
                self->notify_enabled_ = false;
                self->mtu_ = BLE_ATT_MTU_DFLT;  // 协商前默认；等待中心发起交换
                IJPD_LOGI(kTag, "connected handle=%u",
                          static_cast<unsigned>(self->conn_handle_));
            } else {
                start_advertising_();
            }
            return 0;
        case BLE_GAP_EVENT_DISCONNECT:
            IJPD_LOGI(kTag, "disconnected reason=%d", event->disconnect.reason);
            self->conn_handle_ = -1;
            self->notify_enabled_ = false;
            start_advertising_();
            return 0;
        case BLE_GAP_EVENT_MTU:
            self->mtu_ = event->mtu.value;
            IJPD_LOGI(kTag, "mtu %u", static_cast<unsigned>(event->mtu.value));
            return 0;
        case BLE_GAP_EVENT_SUBSCRIBE:
            if (event->subscribe.attr_handle == self->chr_notify_handle_) {
                self->notify_enabled_ = event->subscribe.cur_notify != 0;
                IJPD_LOGI(kTag, "notify %s",
                          self->notify_enabled_ ? "enabled" : "disabled");
            }
            return 0;
        default:
            return 0;
    }
}

int BleControlServer::gatt_write_cb_(uint16_t conn, uint16_t attr,
                                     ble_gatt_access_ctxt* ctxt, void* arg) noexcept {
    (void)conn;
    (void)attr;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;
    BleControlServer* self = g_self;
    if (self == nullptr || self->queue_ == nullptr) return BLE_ATT_ERR_UNLIKELY;

    RxMsg msg{};
    const uint16_t in_len = OS_MBUF_PKTLEN(ctxt->om);
    if (in_len > sizeof(msg.buf)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    uint16_t copied = 0;
    int rc = ble_hs_mbuf_to_flat(ctxt->om, msg.buf, sizeof(msg.buf), &copied);
    if (rc != 0) return BLE_ATT_ERR_UNLIKELY;
    msg.len = copied;

    if (xQueueSendToBack(static_cast<QueueHandle_t>(self->queue_), &msg, 0) != pdTRUE) {
        self->dropped_rx_.fetch_add(1, std::memory_order_relaxed);  // 热路径不打印
    }
    return 0;
}

int BleControlServer::gatt_read_cb_(uint16_t conn, uint16_t attr,
                                    ble_gatt_access_ctxt* ctxt, void* arg) noexcept {
    (void)conn;
    (void)attr;
    (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        const uint8_t val = 0;
        return os_mbuf_append(ctxt->om, &val, 1);
    }
    return 0;
}

void BleControlServer::on_sync_() noexcept {
    if (ble_hs_util_ensure_addr(0) != 0) {
        IJPD_LOGE(kTag, "device address error");
        return;
    }
    start_advertising_();
}

void BleControlServer::on_reset_(int reason) noexcept {
    IJPD_LOGE(kTag, "host reset reason=%d", reason);
}

void BleControlServer::host_task_(void* param) noexcept {
    (void)param;
    nimble_port_run();  // 返回即主机停止
    nimble_port_freertos_deinit();
    vTaskDelete(nullptr);
}

BleControlServer::~BleControlServer() { stop(); }

Err BleControlServer::start(const char* device_name, const Host& host) noexcept {
    if (host.feed == nullptr || host.send == nullptr || device_name == nullptr) {
        return Err::INVALID_ARGUMENT;
    }
    host_ = host;
    g_self = this;

    memcpy(g_svc_uuid.value, kSvcUuidLe, 16);
    memcpy(g_write_uuid.value, kWriteUuidLe, 16);
    memcpy(g_notify_uuid.value, kNotifyUuidLe, 16);
    g_svc_uuid.u.type = BLE_UUID_TYPE_128;
    g_write_uuid.u.type = BLE_UUID_TYPE_128;
    g_notify_uuid.u.type = BLE_UUID_TYPE_128;

    queue_ = xQueueCreate(8, sizeof(RxMsg));
    if (queue_ == nullptr) return Err::OUT_OF_MEMORY;

    tx_lock_ = xSemaphoreCreateMutex();
    if (tx_lock_ == nullptr) return Err::OUT_OF_MEMORY;

    if (nimble_port_init() != ESP_OK) {
        IJPD_LOGE(kTag, "nimble_port_init failed");
        return Err::INTERNAL_ERROR;
    }
    ble_hs_cfg.sync_cb = &BleControlServer::on_sync_;
    ble_hs_cfg.reset_cb = &BleControlServer::on_reset_;

    ble_svc_gap_device_name_set(device_name);

    // GATT 服务表（静态定义，避免 C++ 复合字面量）
    static struct ble_gatt_chr_def chr_defs[3];
    memset(chr_defs, 0, sizeof(chr_defs));
    chr_defs[0].uuid = &g_write_uuid.u;
    chr_defs[0].access_cb = &BleControlServer::gatt_write_cb_;
    chr_defs[0].flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP;
    chr_defs[0].val_handle = &chr_write_handle_;
    chr_defs[1].uuid = &g_notify_uuid.u;
    chr_defs[1].access_cb = &BleControlServer::gatt_read_cb_;
    chr_defs[1].flags = BLE_GATT_CHR_F_NOTIFY;
    chr_defs[1].val_handle = &chr_notify_handle_;

    static struct ble_gatt_svc_def svc_defs[2];
    memset(svc_defs, 0, sizeof(svc_defs));
    svc_defs[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
    svc_defs[0].uuid = &g_svc_uuid.u;
    svc_defs[0].characteristics = chr_defs;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(svc_defs);
    if (rc == 0) rc = ble_gatts_add_svcs(svc_defs);
    if (rc != 0) {
        IJPD_LOGE(kTag, "gatts register rc=%d", rc);
        return Err::INTERNAL_ERROR;
    }

    nimble_port_freertos_init(&BleControlServer::host_task_);
    IJPD_LOGI(kTag, "ble started (%s)", device_name);
    return Err::OK;
}

void BleControlServer::stop() noexcept {
    if (g_self == this) g_self = nullptr;
    conn_handle_ = -1;
    if (queue_ != nullptr) {
        vQueueDelete(static_cast<QueueHandle_t>(queue_));
        queue_ = nullptr;
    }
}

void BleControlServer::poll() noexcept {
    if (queue_ == nullptr) return;
    RxMsg msg;
    while (xQueueReceive(static_cast<QueueHandle_t>(queue_), &msg, 0) == pdTRUE) {
        host_.feed(host_.user, msg.buf, msg.len);
    }
}

Err BleControlServer::send(const uint8_t* data, size_t len) noexcept {
    const int conn = conn_handle_;
    if (conn < 0 || !notify_enabled_) {
        dropped_tx_.fetch_add(1, std::memory_order_relaxed);
        return Err::DATA_CONNECTION_LOST;
    }
    uint16_t chunk = static_cast<uint16_t>(mtu_ - 3);
    if (chunk < cfg::BLE_MIN_NOTIFY_CHUNK) chunk = cfg::BLE_MIN_NOTIFY_CHUNK;

    // 串行化跨任务写:响应(ctrl 任务)与事件广播(report 任务)并发分块
    // notify 会让两帧交错,接收端无法拆帧。整帧发送期间持锁。
    const SemaphoreHandle_t lock = static_cast<SemaphoreHandle_t>(tx_lock_);
    if (lock != nullptr) xSemaphoreTake(lock, portMAX_DELAY);

    size_t sent = 0;
    while (sent < len) {
        const size_t n = (len - sent < chunk) ? (len - sent) : chunk;
        struct os_mbuf* om = ble_hs_mbuf_from_flat(data + sent, n);
        if (om == nullptr) {
            dropped_tx_.fetch_add(1, std::memory_order_relaxed);
            if (lock != nullptr) xSemaphoreGive(lock);
            return Err::OUT_OF_MEMORY;
        }
        const int rc = ble_gatts_notify_custom(static_cast<uint16_t>(conn),
                                               chr_notify_handle_, om);
        if (rc != 0) {
            dropped_tx_.fetch_add(1, std::memory_order_relaxed);
            if (lock != nullptr) xSemaphoreGive(lock);
            return Err::INTERNAL_ERROR;
        }
        sent += n;
    }
    if (lock != nullptr) xSemaphoreGive(lock);
    return Err::OK;
}

}  // namespace ijpd
