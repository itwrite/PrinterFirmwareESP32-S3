// mqtt_adapter.cpp - Optional MQTT control/event adapter.
#include "platform_esp/mqtt_adapter.h"

#include <cstdio>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "mqtt_client.h"

#include "platform_esp/log_adapter.h"

namespace ijpd {
namespace {
constexpr const char* kTag = "mqtt";
constexpr int kQos = 1;
constexpr size_t kCommandQueueDepth = 2;

struct CommandMessage {
    uint16_t len;
    char data[cfg::MAX_CONTROL_FRAME_SIZE + 1];
};
}

MqttAdapter::~MqttAdapter() { stop(); }

Err MqttAdapter::start(const char* uri, const char* base_topic, const char* device_id,
                       const Host& host) noexcept {
    if (uri == nullptr || uri[0] == '\0' || base_topic == nullptr ||
        base_topic[0] == '\0' || device_id == nullptr || host.on_command == nullptr) {
        return Err::INVALID_ARGUMENT;
    }
    if (started_) return Err::OK;

    const int n = snprintf(event_topic_, sizeof(event_topic_), "%s/%s/event",
                           base_topic, device_id);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(event_topic_)) {
        return Err::INVALID_ARGUMENT;
    }
    command_queue_ = xQueueCreate(kCommandQueueDepth, sizeof(CommandMessage));
    if (command_queue_ == nullptr) return Err::OUT_OF_MEMORY;

    esp_mqtt_client_config_t cfg{};
    cfg.broker.address.uri = uri;
    cfg.credentials.client_id = device_id;
    client_ = esp_mqtt_client_init(&cfg);
    if (client_ == nullptr) {
        vQueueDelete(static_cast<QueueHandle_t>(command_queue_));
        command_queue_ = nullptr;
        return Err::OUT_OF_MEMORY;
    }
    host_ = host;
    if (esp_mqtt_client_register_event(
            static_cast<esp_mqtt_client_handle_t>(client_),
            static_cast<esp_mqtt_event_id_t>(ESP_EVENT_ANY_ID),
            reinterpret_cast<esp_event_handler_t>(&MqttAdapter::event_handler_), this) != ESP_OK) {
        esp_mqtt_client_destroy(static_cast<esp_mqtt_client_handle_t>(client_));
        client_ = nullptr;
        vQueueDelete(static_cast<QueueHandle_t>(command_queue_));
        command_queue_ = nullptr;
        return Err::INTERNAL_ERROR;
    }
    if (esp_mqtt_client_start(static_cast<esp_mqtt_client_handle_t>(client_)) != ESP_OK) {
        esp_mqtt_client_destroy(static_cast<esp_mqtt_client_handle_t>(client_));
        client_ = nullptr;
        vQueueDelete(static_cast<QueueHandle_t>(command_queue_));
        command_queue_ = nullptr;
        return Err::INTERNAL_ERROR;
    }
    started_ = true;
    IJPD_LOGI(kTag, "adapter started");
    return Err::OK;
}

void MqttAdapter::stop() noexcept {
    if (client_ != nullptr) {
        auto client = static_cast<esp_mqtt_client_handle_t>(client_);
        (void)esp_mqtt_client_stop(client);
        (void)esp_mqtt_client_destroy(client);
        client_ = nullptr;
    }
    if (command_queue_ != nullptr) {
        vQueueDelete(static_cast<QueueHandle_t>(command_queue_));
        command_queue_ = nullptr;
    }
    connected_pending_.store(false, std::memory_order_relaxed);
    command_len_ = 0;
    started_ = false;
}

void MqttAdapter::poll() noexcept {
    if (command_queue_ == nullptr) return;
    if (connected_pending_.exchange(false, std::memory_order_acq_rel) &&
        host_.on_connected != nullptr) {
        host_.on_connected(host_.user);
    }
    CommandMessage message{};
    while (xQueueReceive(static_cast<QueueHandle_t>(command_queue_), &message, 0) == pdTRUE) {
        if (host_.on_command != nullptr) {
            host_.on_command(host_.user, message.data, message.len);
        }
    }
}

Err MqttAdapter::publish_event(const char* data, size_t len) noexcept {
    if (!started_ || client_ == nullptr || data == nullptr || len > INT32_MAX) {
        return Err::INVALID_ARGUMENT;
    }
    const int id = esp_mqtt_client_publish(
        static_cast<esp_mqtt_client_handle_t>(client_), event_topic_, data,
        static_cast<int>(len), kQos, 0);
    return id < 0 ? Err::DATA_CONNECTION_LOST : Err::OK;
}

void MqttAdapter::event_handler_(void* handler_args, const char*, int32_t event_id,
                                 void* event_data) noexcept {
    auto* self = static_cast<MqttAdapter*>(handler_args);
    if (self != nullptr) self->on_event_(event_id, event_data);
}

void MqttAdapter::on_event_(int32_t event_id, void* event_data) noexcept {
    auto* event = static_cast<esp_mqtt_event_handle_t>(event_data);
    if (event == nullptr) return;
    if (event_id == MQTT_EVENT_CONNECTED) {
        char topic[160] = {};
        // Commands are scoped below the same base as the event topic.
        const char* suffix = "/command";
        const char* slash = std::strrchr(event_topic_, '/');
        if (slash == nullptr) return;
        const size_t base_len = static_cast<size_t>(slash - event_topic_);
        if (base_len + std::strlen(suffix) + 1 > sizeof(topic)) return;
        std::memcpy(topic, event_topic_, base_len);
        std::strcpy(topic + base_len, suffix);
        (void)esp_mqtt_client_subscribe(
            static_cast<esp_mqtt_client_handle_t>(client_), topic, kQos);
        connected_pending_.store(true, std::memory_order_release);
    } else if (event_id == MQTT_EVENT_DATA && event->data != nullptr &&
               event->data_len > 0 && host_.on_command != nullptr) {
        const size_t offset = event->current_data_offset < 0
                                  ? 0
                                  : static_cast<size_t>(event->current_data_offset);
        const size_t len = static_cast<size_t>(event->data_len);
        if (offset == 0) command_len_ = 0;
        if (offset != command_len_ || command_len_ + len > cfg::MAX_CONTROL_FRAME_SIZE) {
            command_len_ = 0;
            return;
        }
        std::memcpy(command_buf_ + command_len_, event->data, len);
        command_len_ += len;
        const size_t total = event->total_data_len > 0
                                 ? static_cast<size_t>(event->total_data_len)
                                 : command_len_;
        if (command_len_ == total) {
            if (command_len_ >= cfg::MAX_CONTROL_FRAME_SIZE) {
                command_len_ = 0;
                return;
            }
            if (command_len_ == 0 || command_buf_[command_len_ - 1] !=
                                         cfg::FRAME_TERMINATOR) {
                command_buf_[command_len_++] = cfg::FRAME_TERMINATOR;
            }
            CommandMessage message{};
            message.len = static_cast<uint16_t>(command_len_);
            std::memcpy(message.data, command_buf_, command_len_);
            (void)xQueueSendToBack(static_cast<QueueHandle_t>(command_queue_),
                                   &message, 0);
            command_len_ = 0;
        }
    }
}

}  // namespace ijpd
