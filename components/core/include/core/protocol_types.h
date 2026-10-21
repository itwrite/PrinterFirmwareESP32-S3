// ============================================================================
// protocol_types.h —— 协议级公共枚举（Phase 3）
//
// start_print（控制通道）与数据通道帧校验共用，避免数据路径反向依赖
// command_router。
// ============================================================================
#ifndef IJPD_CORE_PROTOCOL_TYPES_H
#define IJPD_CORE_PROTOCOL_TYPES_H

#include <cstdint>

namespace ijpd {

enum class Compression : uint8_t { None = 0, Lz4Block = 1 };
enum class Integrity : uint8_t { None = 0, Crc32 = 1 };

}  // namespace ijpd

#endif  // IJPD_CORE_PROTOCOL_TYPES_H
