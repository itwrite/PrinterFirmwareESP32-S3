// test_lz4_block.cpp —— LZ4 Block 解压器：手工向量 / 恶意输入 / 回环
// 覆盖范围（Phase 3 验收要求）：
//   A. 手工向量：纯 literal、literal 长度扩展(15+255 链)、短匹配、长匹配
//      （match 长度扩展链）、offset=1 重叠游程、offset=2 重叠、
//      literal 扩展 + 匹配扩展混合、空流(expected=0)。
//   B. 恶意输入：对每个合法向量在所有位置截断；offset=0；offset>已输出；
//      match/literal 长度越界；输出溢出；末尾粘垃圾；空输入非空期望；
//      随机字节（固定种子 2 万次）；随机变异（10 万次，ASan/UBSan）。
//   C. 回环：TestEncoder（测试专用贪心哈希编码器）对随机/重复/混合数据
//      压缩后解压，逐字节相等。
#include "core/lz4_block.h"
#include "assert.h"

#include <cstring>
#include <vector>

#include "core/byte_order.h"

using namespace ijpd;

namespace {

// ---- 测试专用 LZ4 编码器（仅用于回环验证解码器；固件不含编码器） ----
struct TestEncoder {
    static constexpr size_t kHashLog = 12;
    static constexpr size_t kHashSize = 1 << kHashLog;
    static constexpr size_t kMinMatch = 4;
    static constexpr size_t kMfLimit = 12;

    static uint32_t hash4(const uint8_t* p) noexcept {
        const uint32_t v = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                           (static_cast<uint32_t>(p[2]) << 16) |
                           (static_cast<uint32_t>(p[3]) << 24);
        return (v * 2654435761u) & (kHashSize - 1);
    }

    static void emit(std::vector<uint8_t>& out, const uint8_t* lit, size_t lit_len,
                     size_t offset, size_t m4) noexcept {
        const uint8_t ln = (lit_len < 15) ? static_cast<uint8_t>(lit_len) : 15;
        const uint8_t mn = (m4 < 15) ? static_cast<uint8_t>(m4) : 15;
        out.push_back(static_cast<uint8_t>((ln << 4) | mn));
        if (ln == 15) {
            size_t r = lit_len - 15;
            while (r >= 255) {
                out.push_back(255);
                r -= 255;
            }
            out.push_back(static_cast<uint8_t>(r));
        }
        out.insert(out.end(), lit, lit + lit_len);
        if (offset != 0) {  // offset==0 表示末尾 literals-only 序列
            const size_t at = out.size();
            out.resize(at + 2);
            write_u16_le(out.data() + at, static_cast<uint16_t>(offset));
            if (mn == 15) {
                size_t r = m4 - 15;
                while (r >= 255) {
                    out.push_back(255);
                    r -= 255;
                }
                out.push_back(static_cast<uint8_t>(r));
            }
        }
    }

    static std::vector<uint8_t> encode(const uint8_t* src, size_t n) noexcept {
        std::vector<uint8_t> out;
        std::vector<int32_t> table(kHashSize, -1);
        size_t anchor = 0;
        size_t ip = 0;
        const size_t match_limit = (n >= kMfLimit) ? n - kMfLimit : 0;

        while (ip < match_limit) {
            const uint32_t h = hash4(src + ip);
            const int32_t ref = table[h];
            table[h] = static_cast<int32_t>(ip);
            if (ref >= 0 && ip - static_cast<size_t>(ref) <= 0xFFFF &&
                std::memcmp(src + ref, src + ip, kMinMatch) == 0) {
                size_t ml = kMinMatch;
                while (ip + ml + 1 < n && src[ref + ml] == src[ip + ml]) ++ml;
                emit(out, src + anchor, ip - anchor, ip - static_cast<size_t>(ref),
                     ml - kMinMatch);
                ip += ml;
                anchor = ip;
            } else {
                ++ip;
            }
        }
        emit(out, src + anchor, n - anchor, 0, 0);  // 尾部 literals（>=1）
        return out;
    }
};

uint64_t rng_state = 0x9E3779B97F4A7C15ull;
uint64_t rnd() {
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * 2685821657736338717ull;
}

void fill_dataset(std::vector<uint8_t>& buf, size_t n, int kind) {
    buf.resize(n);
    switch (kind) {
        case 0:  // 随机（不可压缩）
            for (size_t i = 0; i < n; ++i) buf[i] = static_cast<uint8_t>(rnd());
            break;
        case 1:  // 长游程（高度可压缩）
            for (size_t i = 0; i < n; ++i) buf[i] = static_cast<uint8_t>((i / 64) & 0xFF);
            break;
        case 2:  // 混合
            for (size_t i = 0; i < n; ++i) {
                buf[i] = static_cast<uint8_t>((rnd() % 8 == 0) ? (i % 251)
                                                               : (rnd() & 0xFF));
            }
            break;
        case 3:  // 全零
            std::memset(buf.data(), 0, n);
            break;
        default:  // 周期模式
            for (size_t i = 0; i < n; ++i) buf[i] = static_cast<uint8_t>("IJPD"[i % 4]);
            break;
    }
}

}  // namespace

int main() {
    // ================= A. 手工向量 =================
    {
        // V1 纯 literal "AB"
        const uint8_t v1[] = {0x20, 'A', 'B'};
        uint8_t dst[8];
        CHECK_EQ(lz4_block_decompress(v1, sizeof(v1), dst, sizeof(dst), 2), 2);
        CHECK_EQ(dst[0], 'A');
        CHECK_EQ(dst[1], 'B');
    }
    {
        // V2 游程：'A' + offset=1 匹配 4 + 尾部 literal 'B' -> "AAAAAB"
        const uint8_t v2[] = {0x10, 'A', 0x01, 0x00, 0x10, 'B'};
        uint8_t dst[8];
        CHECK_EQ(lz4_block_decompress(v2, sizeof(v2), dst, sizeof(dst), 6), 6);
        CHECK_EQ_MEM(dst, "AAAAAB", 6);
    }
    {
        // V3 offset=2 重叠："AB" + match(off=2,len=6, token 低 4 位=2) -> "ABABABAB" + 尾 "C"
        const uint8_t v3[] = {0x22, 'A', 'B', 0x02, 0x00, 0x10, 'C'};
        uint8_t dst[16];
        CHECK_EQ(lz4_block_decompress(v3, sizeof(v3), dst, sizeof(dst), 9), 9);
        CHECK_EQ_MEM(dst, "ABABABABC", 9);
    }
    {
        // V4 literal 长度扩展：lit=20（15+5）
        std::vector<uint8_t> v;
        v.push_back(0xF0);
        v.push_back(5);  // 15+5=20 literals，无 match（末尾序列）
        for (int i = 0; i < 20; ++i) v.push_back(static_cast<uint8_t>('a' + i % 26));
        uint8_t dst[32];
        CHECK_EQ(lz4_block_decompress(v.data(), v.size(), dst, sizeof(dst), 20), 20);
        CHECK_EQ(dst[0], 'a');
        CHECK_EQ(dst[19], 't');
    }
    {
        // V5 长匹配扩展：'X' + match(off=1) len = 4+15+255+10 = 284，尾 lit 'Y'
        std::vector<uint8_t> v;
        v.push_back(0x1F);  // 1 literal, matchlen 扩展
        v.push_back('X');
        v.push_back(0x01);
        v.push_back(0x00);  // offset=1
        v.push_back(255);   // 扩展链：15 -> +255 继续
        v.push_back(10);    // +10 -> matchlen = 4+15+255+10 = 284
        v.push_back(0x10);  // 尾 literal
        v.push_back('Y');
        const size_t total = 1 + 284 + 1;
        std::vector<uint8_t> dst(total);
        CHECK_EQ(lz4_block_decompress(v.data(), v.size(), dst.data(), total, total),
                 static_cast<int>(total));
        CHECK_EQ(dst[0], 'X');
        CHECK_EQ(dst[283], 'X');
        CHECK_EQ(dst[284], 'X');  // match 覆盖 [1, 284]
        CHECK_EQ(dst[285], 'Y');
    }
    {
        // V6 literal 扩展 255 链：lit = 15+255+255+3 = 528
        std::vector<uint8_t> v;
        v.push_back(0xF0);
        v.push_back(255);
        v.push_back(255);
        v.push_back(3);
        std::vector<uint8_t> src(528);
        for (size_t i = 0; i < src.size(); ++i) src[i] = static_cast<uint8_t>(i & 0x7F);
        v.insert(v.end(), src.begin(), src.end());
        std::vector<uint8_t> dst(528);
        CHECK_EQ(lz4_block_decompress(v.data(), v.size(), dst.data(), 528, 528), 528);
        CHECK_EQ(dst[527], static_cast<int>(527 & 0x7F));
    }
    {
        // V7 空流：expected=0, src 长度 0（nullptr 由防御检查拒绝）
        uint8_t dst[1];
        const uint8_t dummy = 0;
        CHECK_EQ(lz4_block_decompress(&dummy, 0, dst, 1, 0), 0);
        CHECK_EQ(lz4_block_decompress(nullptr, 0, dst, 1, 0), kLz4Corrupt);
    }

    // ================= B. 恶意输入 =================
    {
        // 全位置截断：V5 向量任何前缀都不是完整合法流（或长度不符）
        std::vector<uint8_t> v;
        v.push_back(0x1F);
        v.push_back('X');
        v.push_back(0x01);
        v.push_back(0x00);
        v.push_back(255);
        v.push_back(10);
        v.push_back(0x10);
        v.push_back('Y');
        const size_t total = 286;
        std::vector<uint8_t> dst(total);
        for (size_t cut = 0; cut < v.size(); ++cut) {
            const int r = lz4_block_decompress(v.data(), cut, dst.data(), total, total);
            CHECK_TRUE(r < 0);
        }
    }
    {
        const uint8_t bad_offset0[] = {0x10, 'A', 0x00, 0x00, 0x10, 'B'};
        uint8_t dst[8];
        CHECK_EQ(lz4_block_decompress(bad_offset0, sizeof(bad_offset0), dst, 8, 6),
                 kLz4Corrupt);

        const uint8_t bad_offset_far[] = {0x10, 'A', 0x05, 0x00, 0x10, 'B'};
        CHECK_EQ(lz4_block_decompress(bad_offset_far, sizeof(bad_offset_far), dst, 8, 6),
                 kLz4Corrupt);

        // match 越过 expected：matchlen 6 但 expected 5
        const uint8_t over[] = {0x10, 'A', 0x01, 0x00, 0x10, 'B'};
        CHECK_EQ(lz4_block_decompress(over, sizeof(over), dst, 8, 5), kLz4Corrupt);

        // dst_cap 不足
        CHECK_EQ(lz4_block_decompress(over, sizeof(over), dst, 2, 6), kLz4Corrupt);

        // 末尾粘垃圾
        std::vector<uint8_t> tail_bad = {0x10, 'A', 0x01, 0x00, 0x10, 'B'};
        tail_bad.push_back(0x00);
        CHECK_EQ(lz4_block_decompress(tail_bad.data(), tail_bad.size(), dst, 8, 6),
                 kLz4Corrupt);

        // literal 声称超过输入
        const uint8_t lit_over[] = {0xF0, 200, 'x'};
        CHECK_EQ(lz4_block_decompress(lit_over, sizeof(lit_over), dst, 8, 205),
                 kLz4Corrupt);

        // 空输入非空期望
        CHECK_EQ(lz4_block_decompress(nullptr, 0, dst, 8, 4), kLz4Corrupt);

        // 长度不符（正确解码 6 字节，期望 7）
        const uint8_t ok6[] = {0x10, 'A', 0x01, 0x00, 0x10, 'B'};
        CHECK_EQ(lz4_block_decompress(ok6, sizeof(ok6), dst, 8, 7), kLz4LengthMismatch);
    }
    {
        // 随机字节 2 万次 + 随机变异 10 万次（ASan/UBSan 下无崩溃、无越界）
        std::vector<uint8_t> src(256);
        std::vector<uint8_t> dst(256);
        for (int it = 0; it < 20000; ++it) {
            const size_t n = 1 + rnd() % 200;
            for (size_t i = 0; i < n; ++i) src[i] = static_cast<uint8_t>(rnd());
            const int r = lz4_block_decompress(src.data(), n, dst.data(), 256, 256);
            CHECK_TRUE(r <= 256);
        }
        // 变异：合法压缩流改写/截断（ASan/UBSan 下无崩溃、无越界；
        // 解码器只有 d==expected 才返回正值，故正值必为 300）
        std::vector<uint8_t> data(300);
        fill_dataset(data, data.size(), 2);
        std::vector<uint8_t> mdst(320);
        const std::vector<uint8_t> comp = TestEncoder::encode(data.data(), data.size());
        for (int it = 0; it < 100000; ++it) {
            std::vector<uint8_t> mut = comp;
            const int nmut = 1 + static_cast<int>(rnd() % 4);
            for (int m = 0; m < nmut && !mut.empty(); ++m) {
                switch (rnd() % 3) {
                    case 0:
                        mut[rnd() % mut.size()] = static_cast<uint8_t>(rnd());
                        break;
                    case 1:
                        mut.resize(rnd() % (mut.size() + 1));
                        break;
                    default:
                        mut.push_back(static_cast<uint8_t>(rnd()));
                        break;
                }
            }
            const int r =
                lz4_block_decompress(mut.data(), mut.size(), mdst.data(), mdst.size(), 300);
            CHECK_TRUE(r < 0 || r == 300);
        }
    }

    // ================= C. 回环 =================
    {
        const size_t sizes[] = {1, 2, 3, 4, 11, 12, 13, 15, 16, 100, 255, 256, 1000,
                                4096, 10000, 65535};
        for (int kind = 0; kind < 5; ++kind) {
            for (size_t n : sizes) {
                std::vector<uint8_t> data;
                fill_dataset(data, n, kind);
                const std::vector<uint8_t> comp = TestEncoder::encode(data.data(), n);
                std::vector<uint8_t> out(n);
                const int r = lz4_block_decompress(comp.data(), comp.size(), out.data(),
                                                   n, n);
                CHECK_EQ(r, static_cast<int>(n));
                CHECK_EQ_MEM(out.data(), data.data(), n);
            }
        }
        // 重复运行编码器稳定性（游程数据应显著压缩）
        std::vector<uint8_t> runs;
        fill_dataset(runs, 65535, 1);
        const std::vector<uint8_t> c = TestEncoder::encode(runs.data(), runs.size());
        CHECK_TRUE(c.size() < runs.size() / 4);
    }

    return ::ijpd::test::summary("test_lz4_block");
}
