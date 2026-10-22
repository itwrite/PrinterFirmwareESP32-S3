// test_mqtt_chunk_protocol.cpp - MQTT job chunk bounds/CRC validation.
#include "core/mqtt_chunk_protocol.h"
#include "assert.h"

#include <cstring>
#include <vector>

#include "core/byte_order.h"

using namespace ijpd;

namespace {
std::vector<uint8_t> chunk(const char* job, uint32_t index, uint32_t total,
                           const std::vector<uint8_t>& payload) {
    const size_t job_len = std::strlen(job);
    std::vector<uint8_t> out(MQTT_CHUNK_HEADER_SIZE + job_len + payload.size(), 0);
    std::memcpy(out.data(), "IJCH", 4);
    out[4] = MQTT_CHUNK_VERSION;
    out[6] = static_cast<uint8_t>(job_len);
    write_u32_le(out.data() + 8, index);
    write_u32_le(out.data() + 12, total);
    write_u32_le(out.data() + 16, static_cast<uint32_t>(payload.size()));
    write_u32_le(out.data() + 20, crc32(payload.data(), payload.size()));
    std::memcpy(out.data() + MQTT_CHUNK_HEADER_SIZE, job, job_len);
    std::memcpy(out.data() + MQTT_CHUNK_HEADER_SIZE + job_len,
                payload.data(), payload.size());
    return out;
}
}  // namespace

int main() {
    const std::vector<uint8_t> payload = {1, 2, 3, 4};
    const std::vector<uint8_t> bytes = chunk("job-1", 0, 2, payload);
    MqttChunkView view{};
    CHECK_TRUE(parse_mqtt_chunk(bytes.data(), bytes.size(), &view) == Err::OK);
    CHECK_EQ(view.job_id_len, 5u);
    CHECK_EQ(view.chunk_index, 0u);
    CHECK_EQ(view.total_chunks, 2u);
    CHECK_EQ(view.payload_len, 4u);
    CHECK_EQ_MEM(view.payload, payload.data(), payload.size());

    std::vector<uint8_t> bad_crc = bytes;
    bad_crc.back() ^= 1;
    CHECK_TRUE(parse_mqtt_chunk(bad_crc.data(), bad_crc.size(), &view) ==
               Err::PACKET_CRC_MISMATCH);

    std::vector<uint8_t> bad_index = bytes;
    write_u32_le(bad_index.data() + 8, 2);
    CHECK_TRUE(parse_mqtt_chunk(bad_index.data(), bad_index.size(), &view) ==
               Err::PACKET_LENGTH_INVALID);

    std::vector<uint8_t> bad_magic = bytes;
    bad_magic[0] = 'X';
    CHECK_TRUE(parse_mqtt_chunk(bad_magic.data(), bad_magic.size(), &view) ==
               Err::INVALID_ARGUMENT);
    return ::ijpd::test::summary("test_mqtt_chunk_protocol");
}
