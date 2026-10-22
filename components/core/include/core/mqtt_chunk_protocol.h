// ============================================================================
// mqtt_chunk_protocol.h - Bounded MQTT job chunk validation.
//
// MQTT payload format (binary, little endian):
//   magic[4] = "IJCH"
//   version u8 = 1
//   flags u8 (reserved, must be 0)
//   job_id_len u8 (1..40)
//   reserved u8 (0)
//   chunk_index u32
//   total_chunks u32
//   payload_len u32
//   payload_crc32 u32 (CRC32 of payload only)
//   job_id bytes
//   payload bytes
// ============================================================================
#ifndef IJPD_CORE_MQTT_CHUNK_PROTOCOL_H
#define IJPD_CORE_MQTT_CHUNK_PROTOCOL_H

#include <cstddef>
#include <cstdint>

#include "core/config_limits.h"
#include "core/crc32.h"
#include "core/errors.h"

namespace ijpd {

struct MqttChunkView {
    const char* job_id = nullptr;
    uint8_t job_id_len = 0;
    uint32_t chunk_index = 0;
    uint32_t total_chunks = 0;
    const uint8_t* payload = nullptr;
    uint32_t payload_len = 0;
};

class MqttChunkSequence {
public:
    Err begin(const char* job_id, size_t job_id_len) noexcept;
    Err accept(const MqttChunkView& chunk) noexcept;
    void reset() noexcept;
    bool active() const noexcept { return active_; }
    bool complete() const noexcept { return active_ && next_index_ == total_chunks_; }
    uint32_t next_index() const noexcept { return next_index_; }
    uint32_t total_chunks() const noexcept { return total_chunks_; }

private:
    char job_id_[cfg::JOB_ID_MAX + 1] = {};
    uint8_t job_id_len_ = 0;
    uint32_t total_chunks_ = 0;
    uint32_t next_index_ = 0;
    bool active_ = false;
};

inline constexpr size_t MQTT_CHUNK_HEADER_SIZE = 24;
inline constexpr uint8_t MQTT_CHUNK_VERSION = 1;

Err parse_mqtt_chunk(const uint8_t* data, size_t len, MqttChunkView* out) noexcept;

}  // namespace ijpd

#endif  // IJPD_CORE_MQTT_CHUNK_PROTOCOL_H
