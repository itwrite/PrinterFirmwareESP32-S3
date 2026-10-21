// test_crc32.cpp —— CRC-32/ISO-HDLC 标准向量与流式一致性
// 向量经 Python zlib 独立生成（Phase 1 会话内验证）。
#include "core/crc32.h"
#include "assert.h"

using namespace ijpd;

int main() {
    // ---- 标准向量 ----
    CHECK_EQ_HEX(crc32(nullptr, 0), 0x00000000u);          // 空输入
    CHECK_EQ_HEX(crc32(reinterpret_cast<const uint8_t*>("123456789"), 9),
                 0xCBF43926u);                              // CRC-32/ISO-HDLC 标准向量
    CHECK_EQ_HEX(crc32(reinterpret_cast<const uint8_t*>(
                 "The quick brown fox jumps over the lazy dog"), 43),
                 0x414FA339u);
    {
        uint8_t all[256];
        for (int i = 0; i < 256; ++i) all[i] = static_cast<uint8_t>(i);
        CHECK_EQ_HEX(crc32(all, sizeof(all)), 0x29058C73u);
    }
    // 协议文档 §7.1 示例：6B 头 + 16B 载荷
    {
        const uint8_t frame[] = {0x00, 0x00, 0x10, 0x00, 0x10, 0x00,
                                 '0', '1', '2', '3', '4', '5', '6', '7',
                                 '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
        CHECK_EQ_HEX(crc32(frame, sizeof(frame)), 0x12CB3A3Du);
    }

    // ---- 流式 == 一次性 ----
    {
        const char* fox = "The quick brown fox jumps over the lazy dog";
        const size_t n = 43;
        const size_t splits[] = {1, 3, 0, 17, 2, 20};  // 0 表示空块
        uint32_t state = CRC32_INIT_STATE;
        size_t pos = 0;
        for (size_t s : splits) {
            if (pos + s > n) s = n - pos;
            state = crc32_update(state, reinterpret_cast<const uint8_t*>(fox) + pos, s);
            pos += s;
        }
        state = crc32_update(state, reinterpret_cast<const uint8_t*>(fox) + pos, n - pos);
        CHECK_EQ_HEX(crc32_final(state), crc32(reinterpret_cast<const uint8_t*>(fox), n));
    }

    // ---- 单比特翻转必改变结果 ----
    {
        uint8_t buf[9] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
        const uint32_t base = crc32(buf, sizeof(buf));
        buf[0] = static_cast<uint8_t>(buf[0] ^ 0x01);
        CHECK_TRUE(crc32(buf, sizeof(buf)) != base);
    }

    // ---- 逐字节流式 ----
    {
        uint8_t buf[256];
        for (int i = 0; i < 256; ++i) buf[i] = static_cast<uint8_t>(i * 7 + 3);
        uint32_t state = CRC32_INIT_STATE;
        for (size_t i = 0; i < sizeof(buf); ++i)
            state = crc32_update(state, buf + i, 1);
        CHECK_EQ_HEX(crc32_final(state), crc32(buf, sizeof(buf)));
    }

    return ::ijpd::test::summary("test_crc32");
}
