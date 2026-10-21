// ============================================================================
// byte_order.h —— Little Endian 显式读写（Phase 3）
//
// 协议规定所有多字节整数统一 Little Endian；禁止通过指针强转读取
//（避免对齐与字节序问题）。全 core 逐处使用本头文件。
// ============================================================================
#ifndef IJPD_CORE_BYTE_ORDER_H
#define IJPD_CORE_BYTE_ORDER_H

#include <cstdint>

namespace ijpd {

inline uint16_t read_u16_le(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) |
                                 (static_cast<uint16_t>(p[1]) << 8));
}

inline uint32_t read_u32_le(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline void write_u16_le(uint8_t* p, uint16_t v) noexcept {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>(v >> 8);
}

inline void write_u32_le(uint8_t* p, uint32_t v) noexcept {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

}  // namespace ijpd

#endif  // IJPD_CORE_BYTE_ORDER_H
