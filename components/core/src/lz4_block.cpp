// lz4_block.cpp —— LZ4 Block 解压（迭代实现，本文件即“慢速参考路径”；
// 后续如引入 32 位宽拷贝优化，须保留本路径用于对照测试）
#include "core/lz4_block.h"

#include "core/byte_order.h"

namespace ijpd {

int lz4_block_decompress(const uint8_t* src, size_t src_len,
                         uint8_t* dst, size_t dst_cap,
                         size_t expected_len) noexcept {
    if (src == nullptr || dst == nullptr) return kLz4Corrupt;
    if (expected_len > dst_cap) return kLz4Corrupt;  // 输出不可能容纳
    if (expected_len == 0) return (src_len == 0) ? 0 : kLz4Corrupt;

    size_t s = 0;  // 读游标
    size_t d = 0;  // 写游标

    for (;;) {
        // ---- token ----
        if (s >= src_len) return kLz4Corrupt;
        const uint8_t token = src[s++];

        // ---- literal 长度（含 15 扩展链）----
        size_t lit = token >> 4;
        if (lit == 15) {
            uint8_t b = 0;
            do {
                if (s >= src_len) return kLz4Corrupt;
                b = src[s++];
                lit += b;  // 累计 <= src_len*255，size_t 无溢出；下面有界检查
            } while (b == 255);
        }

        // ---- 拷贝 literal ----
        if (lit > src_len - s) return kLz4Corrupt;   // 输入不足
        if (lit > expected_len - d) return kLz4Corrupt;  // 输出越界（提前拒绝）
        for (size_t i = 0; i < lit; ++i) dst[d + i] = src[s + i];
        s += lit;
        d += lit;

        // ---- 最后一个序列：仅 literal，输入恰好耗尽 ----
        if (s == src_len) break;

        // ---- offset ----
        if (src_len - s < 2) return kLz4Corrupt;
        const size_t offset = read_u16_le(src + s);
        s += 2;
        if (offset == 0 || offset > d) return kLz4Corrupt;

        // ---- match 长度（含 15 扩展链）----
        size_t mlen = static_cast<size_t>(token & 0x0F) + 4;
        if ((token & 0x0F) == 15) {
            uint8_t b = 0;
            do {
                if (s >= src_len) return kLz4Corrupt;
                b = src[s++];
                mlen += b;
                if (mlen > expected_len) return kLz4Corrupt;  // 上限早退
            } while (b == 255);
        }
        if (d + mlen > expected_len) return kLz4Corrupt;

        // ---- 匹配拷贝（重叠安全：逐字节，不可 memcpy/memmove）----
        const size_t msrc = d - offset;
        for (size_t i = 0; i < mlen; ++i) dst[d + i] = dst[msrc + i];
        d += mlen;
    }

    if (s != src_len) return kLz4Corrupt;        // 防御（break 时恒相等）
    if (d != expected_len) return kLz4LengthMismatch;
    return static_cast<int>(d);
}

}  // namespace ijpd
