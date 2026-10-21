// ============================================================================
// crc32.h —— CRC-32/ISO-HDLC（zlib/PKZIP 一致）（Phase 1）
//
// 参数：多项式 0x04C11DB7（反射 0xEDB88320）、初值 0xFFFFFFFF、
//       输入/输出反射、终异或 0xFFFFFFFF。
// 标准向量："123456789" -> 0xCBF43926
//
// 线程安全性：纯函数，线程安全。
// 所有权：    无。
// 最大耗时：  ~len/4 条表查找，2KB 输入 < 10us @240MHz。
// ============================================================================
#ifndef IJPD_CORE_CRC32_H
#define IJPD_CORE_CRC32_H

#include <cstddef>
#include <cstdint>

namespace ijpd {

inline constexpr uint32_t CRC32_INIT_STATE = 0xFFFFFFFFu;

// 流式更新。state 初值传 CRC32_INIT_STATE；返回值继续传给下一次调用。
// 注意：返回的是“原始状态”，最终 CRC 需再经过 crc32_final()。
uint32_t crc32_update(uint32_t state, const uint8_t* data, size_t len) noexcept;

inline uint32_t crc32_final(uint32_t state) noexcept { return state ^ 0xFFFFFFFFu; }

// 一次性计算（等价于 final(update(init(), data, len))）
uint32_t crc32(const uint8_t* data, size_t len) noexcept;

}  // namespace ijpd

#endif  // IJPD_CORE_CRC32_H
