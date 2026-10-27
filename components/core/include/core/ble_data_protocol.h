// ble_data_protocol.h - Small ACK envelope for the BLE IJPD data transport.
#ifndef IJPD_CORE_BLE_DATA_PROTOCOL_H
#define IJPD_CORE_BLE_DATA_PROTOCOL_H

#include <cstddef>
#include <cstdint>

#include "core/byte_order.h"

namespace ijpd {
namespace ble_data {

inline constexpr uint8_t kMagic[4] = {'I', 'J', 'B', 'D'};
inline constexpr uint8_t kVersion = 1;
inline constexpr uint8_t kHelloAck = 1;
inline constexpr uint8_t kPacketAck = 2;
inline constexpr uint8_t kError = 3;
inline constexpr uint8_t kOk = 0;
inline constexpr uint8_t kInvalid = 1;
inline constexpr size_t kHeaderBytes = 8;  // magic + version + type + payload length
inline constexpr uint8_t kWindowPackets = 4;
inline constexpr uint16_t kLogicalPacketBytes = 1024;
inline constexpr uint64_t kMaxJobBytes = 512ull * 1024ull;

inline bool has_magic(const uint8_t* data, size_t length) noexcept {
    return data != nullptr && length >= kHeaderBytes && data[0] == kMagic[0] &&
           data[1] == kMagic[1] && data[2] == kMagic[2] && data[3] == kMagic[3] &&
           data[4] == kVersion;
}

inline size_t write_header(uint8_t* out, size_t capacity, uint8_t type,
                           uint16_t payload_length) noexcept {
    if (out == nullptr || capacity < kHeaderBytes) return 0;
    out[0] = kMagic[0];
    out[1] = kMagic[1];
    out[2] = kMagic[2];
    out[3] = kMagic[3];
    out[4] = kVersion;
    out[5] = type;
    write_u16_le(out + 6, payload_length);
    return kHeaderBytes;
}

inline size_t make_hello_ack(uint8_t* out, size_t capacity, uint32_t token,
                             uint16_t packet_bytes) noexcept {
    if (capacity < kHeaderBytes + 8) return 0;
    write_header(out, capacity, kHelloAck, 8);
    write_u32_le(out + kHeaderBytes, token);
    write_u16_le(out + kHeaderBytes + 4, packet_bytes);
    out[kHeaderBytes + 6] = kWindowPackets;
    out[kHeaderBytes + 7] = kOk;
    return kHeaderBytes + 8;
}

inline size_t make_packet_ack(uint8_t* out, size_t capacity, uint16_t index,
                              uint8_t status = kOk) noexcept {
    if (capacity < kHeaderBytes + 4) return 0;
    write_header(out, capacity, kPacketAck, 4);
    write_u16_le(out + kHeaderBytes, index);
    out[kHeaderBytes + 2] = status;
    out[kHeaderBytes + 3] = kWindowPackets;
    return kHeaderBytes + 4;
}

inline size_t make_error(uint8_t* out, size_t capacity, uint8_t status) noexcept {
    if (capacity < kHeaderBytes + 2) return 0;
    write_header(out, capacity, kError, 2);
    out[kHeaderBytes] = status;
    out[kHeaderBytes + 1] = kWindowPackets;
    return kHeaderBytes + 2;
}

}  // namespace ble_data
}  // namespace ijpd

#endif  // IJPD_CORE_BLE_DATA_PROTOCOL_H
