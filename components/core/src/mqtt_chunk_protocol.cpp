// mqtt_chunk_protocol.cpp - Bounded MQTT job chunk validation.
#include "core/mqtt_chunk_protocol.h"

#include <cstring>

#include "core/byte_order.h"

namespace ijpd {

Err MqttChunkSequence::begin(const char* job_id, size_t job_id_len) noexcept {
    if (job_id == nullptr || job_id_len == 0 || job_id_len > cfg::JOB_ID_MAX) {
        return Err::INVALID_ARGUMENT;
    }
    std::memcpy(job_id_, job_id, job_id_len);
    job_id_[job_id_len] = '\0';
    job_id_len_ = static_cast<uint8_t>(job_id_len);
    total_chunks_ = 0;
    next_index_ = 0;
    active_ = true;
    return Err::OK;
}

void MqttChunkSequence::reset() noexcept {
    active_ = false;
    total_chunks_ = 0;
    next_index_ = 0;
    job_id_len_ = 0;
    job_id_[0] = '\0';
}

Err MqttChunkSequence::accept(const MqttChunkView& chunk) noexcept {
    if (!active_ || chunk.job_id == nullptr || chunk.job_id_len != job_id_len_ ||
        std::memcmp(chunk.job_id, job_id_, job_id_len_) != 0) {
        return Err::INVALID_ARGUMENT;
    }
    if (total_chunks_ == 0) total_chunks_ = chunk.total_chunks;
    if (chunk.total_chunks != total_chunks_ || chunk.chunk_index != next_index_) {
        return Err::PACKET_OUT_OF_ORDER;
    }
    ++next_index_;
    return Err::OK;
}

Err parse_mqtt_chunk(const uint8_t* data, size_t len, MqttChunkView* out) noexcept {
    if (data == nullptr || out == nullptr || len < MQTT_CHUNK_HEADER_SIZE) {
        return Err::INVALID_ARGUMENT;
    }
    if (std::memcmp(data, "IJCH", 4) != 0 || data[4] != MQTT_CHUNK_VERSION ||
        data[5] != 0 || data[7] != 0) {
        return Err::INVALID_ARGUMENT;
    }
    const uint8_t job_len = data[6];
    const uint32_t index = read_u32_le(data + 8);
    const uint32_t total = read_u32_le(data + 12);
    const uint32_t payload_len = read_u32_le(data + 16);
    const uint32_t expected_crc = read_u32_le(data + 20);
    if (job_len == 0 || job_len > cfg::JOB_ID_MAX || total == 0 || index >= total ||
        payload_len > cfg::MAX_PACKET_UNCOMPRESSED_SIZE ||
        MQTT_CHUNK_HEADER_SIZE + job_len + payload_len != len) {
        return Err::PACKET_LENGTH_INVALID;
    }
    const char* job_id = reinterpret_cast<const char*>(data + MQTT_CHUNK_HEADER_SIZE);
    for (uint8_t i = 0; i < job_len; ++i) {
        const char c = job_id[i];
        const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!safe) return Err::INVALID_ARGUMENT;
    }
    const uint8_t* payload = data + MQTT_CHUNK_HEADER_SIZE + job_len;
    if (crc32(payload, payload_len) != expected_crc) return Err::PACKET_CRC_MISMATCH;

    out->job_id = job_id;
    out->job_id_len = job_len;
    out->chunk_index = index;
    out->total_chunks = total;
    out->payload = payload;
    out->payload_len = payload_len;
    return Err::OK;
}

}  // namespace ijpd
