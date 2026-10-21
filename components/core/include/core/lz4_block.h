// ============================================================================
// lz4_block.h —— 自研 LZ4 Block 解压器（Phase 3，规范见任务书 6.4）
//
// 实现公开的 LZ4 Block 序列格式（非 LZ4 Frame）：
//   token 高 4 位 = literal 长度，低 4 位 = matchlen−4；值为 15 时用后续
//   字节累加扩展（扩展字节为 255 则继续）。序列 = token | lit_ext |
//   literals | offset(2B LE) | match_ext；最后一个序列只含 literal。
//
// 安全保证：
//   - 每次读取（token/扩展字节/literal/offset）前检查剩余输入；
//   - 每次写入前检查剩余输出（以 expected_len 为界）；
//   - 长度累加带上限早退，无整数溢出路径；
//   - offset == 0 或 offset > 已输出字节数 → 拒绝；
//   - 匹配复制逐字节进行（重叠安全，offset < length 正确）；
//   - 无递归；最坏耗时 O(expected_len)，expected=65535 时 < 1ms @240MHz。
//
// 线程安全性：纯函数，线程安全。
// 所有权：    不持有任何资源；dst 直接写入槽位缓冲（避免二次拷贝）。
// ============================================================================
#ifndef IJPD_CORE_LZ4_BLOCK_H
#define IJPD_CORE_LZ4_BLOCK_H

#include <cstddef>
#include <cstdint>

#include "core/errors.h"

namespace ijpd {

// lz4_block_decompress 的负返回值
inline constexpr int kLz4Corrupt        = -1;  // -> Err::DECOMPRESSION_FAILED
inline constexpr int kLz4LengthMismatch = -2;  // -> Err::DECOMPRESSED_LENGTH_MISMATCH

// 成功返回解压后字节数（== expected_len）；
// 失败返回 kLz4Corrupt / kLz4LengthMismatch。
// 绝不写出 dst + expected_len 之外，绝不读取 src + src_len 之外。
int lz4_block_decompress(const uint8_t* src, size_t src_len,
                         uint8_t* dst, size_t dst_cap,
                         size_t expected_len) noexcept;

// 返回值 -> 协议错误码
inline Err lz4_err_of(int r) noexcept {
    if (r >= 0) return Err::OK;
    return (r == kLz4LengthMismatch) ? Err::DECOMPRESSED_LENGTH_MISMATCH
                                     : Err::DECOMPRESSION_FAILED;
}

}  // namespace ijpd

#endif  // IJPD_CORE_LZ4_BLOCK_H
