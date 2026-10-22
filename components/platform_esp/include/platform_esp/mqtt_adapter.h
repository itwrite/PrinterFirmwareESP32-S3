// ============================================================================
// mqtt_adapter.h - Optional MQTT command/event adapter foundation.
//
// The adapter is disabled unless PrinterConfig.services.mqtt is enabled and a
// broker URI/base topic are configured. It carries control JSON only; large
// print payloads remain on a bounded job data path.
// ============================================================================
#ifndef IJPD_PLATFORM_MQTT_ADAPTER_H
#define IJPD_PLATFORM_MQTT_ADAPTER_H

#include <cstddef>
#include <atomic>

#include "core/config_limits.h"
#include "core/errors.h"

namespace ijpd {

class MqttAdapter {
public:
    struct Host {
        void* user = nullptr;
        void (*on_command)(void* user, const char* data, size_t len) = nullptr;
        void (*on_chunk)(void* user, const uint8_t* data, size_t len) = nullptr;
        void (*on_connected)(void* user) = nullptr;
    };

    Err start(const char* uri, const char* base_topic, const char* device_id,
              const Host& host) noexcept;
    void stop() noexcept;
    void poll() noexcept;
    Err publish_event(const char* data, size_t len) noexcept;

    bool started() const noexcept { return started_; }

    MqttAdapter() noexcept = default;
    ~MqttAdapter();

private:
    static void event_handler_(void* handler_args, const char* base, int32_t event_id,
                               void* event_data) noexcept;
    void on_event_(int32_t event_id, void* event_data) noexcept;

    void* client_ = nullptr;  // esp_mqtt_client_handle_t
    void* command_queue_ = nullptr;  // QueueHandle_t
    Host host_{};
    char event_topic_[160] = {};
    char command_buf_[cfg::MAX_CONTROL_FRAME_SIZE + 1] = {};
    size_t command_len_ = 0;
    std::atomic<bool> connected_pending_{false};
    void* chunk_queue_ = nullptr;  // QueueHandle_t
    void* chunk_buffers_[2] = {nullptr, nullptr};
    std::atomic<uint8_t> chunk_free_mask_{0};
    int chunk_slot_ = -1;
    size_t chunk_len_ = 0;
    bool message_is_chunk_ = false;
    bool message_discard_ = false;
    bool started_ = false;
};

}  // namespace ijpd

#endif  // IJPD_PLATFORM_MQTT_ADAPTER_H
