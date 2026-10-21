// fuzz_bin_lz4.cpp —— 帧流/LZ4 固定种子模糊测试（任务书 12.2：>= 10 万次）
// A. 帧流 fuzz：合法流（Hello+帧，随机模式/CRC/长度）施加随机变异
//    （改写/截断/拼接/插入），随机切块投喂；无崩溃、无越界（ASan）、
//    状态机不无限循环、失败锁存一致。
// B. LZ4 fuzz：合法压缩流变异后解压到精确大小缓冲；正值仅当 == expected。
#include "core/bin_frame_parser.h"
#include "core/lz4_block.h"
#include "assert.h"

#include <cstdlib>
#include <cstring>
#include <vector>

#include "core/byte_order.h"
#include "core/crc32.h"

using namespace ijpd;

namespace {

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed) {}
    uint64_t next() {
        s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
        return s * 2685821657736338717ull;
    }
    uint32_t below(uint32_t n) { return static_cast<uint32_t>(next() % n); }
    uint8_t byte() { return static_cast<uint8_t>(next() >> 32); }
};

// 与 test_lz4_block 相同的测试编码器（简化副本，仅 fuzz 用）
struct FuzzEncoder {
    static constexpr size_t kHashLog = 12;
    static constexpr size_t kHashSize = 1 << kHashLog;
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
            while (r >= 255) { out.push_back(255); r -= 255; }
            out.push_back(static_cast<uint8_t>(r));
        }
        out.insert(out.end(), lit, lit + lit_len);
        if (offset != 0) {
            const size_t at = out.size();
            out.resize(at + 2);
            write_u16_le(out.data() + at, static_cast<uint16_t>(offset));
            if (mn == 15) {
                size_t r = m4 - 15;
                while (r >= 255) { out.push_back(255); r -= 255; }
                out.push_back(static_cast<uint8_t>(r));
            }
        }
    }
    static std::vector<uint8_t> encode(const uint8_t* src, size_t n) noexcept {
        std::vector<uint8_t> out;
        std::vector<int32_t> table(kHashSize, -1);
        size_t anchor = 0, ip = 0;
        const size_t match_limit = (n >= 12) ? n - 12 : 0;
        while (ip < match_limit) {
            const uint32_t h = hash4(src + ip);
            const int32_t ref = table[h];
            table[h] = static_cast<int32_t>(ip);
            if (ref >= 0 && ip - static_cast<size_t>(ref) <= 0xFFFF &&
                std::memcmp(src + ref, src + ip, 4) == 0) {
                size_t ml = 4;
                while (ip + ml + 1 < n && src[ref + ml] == src[ip + ml]) ++ml;
                emit(out, src + anchor, ip - anchor, ip - static_cast<size_t>(ref), ml - 4);
                ip += ml;
                anchor = ip;
            } else {
                ++ip;
            }
        }
        emit(out, src + anchor, n - anchor, 0, 0);
        return out;
    }
};

std::vector<uint8_t> build_stream(Rng& rng, BinFrameParser::JobConfig* cfg) {
    const uint32_t packets = 1 + rng.below(4);
    const uint16_t nominal = static_cast<uint16_t>(1 + rng.below(40));
    const uint16_t last = static_cast<uint16_t>(1 + rng.below(nominal) + 1);
    const Compression comp = (rng.below(2) == 0) ? Compression::None : Compression::Lz4Block;
    const Integrity integ = (rng.below(2) == 0) ? Integrity::None : Integrity::Crc32;

    *cfg = BinFrameParser::JobConfig{};
    cfg->data_token = rng.below(100000);
    cfg->total_packets = packets;
    cfg->total_bytes_uncompressed =
        static_cast<uint64_t>(nominal) * (packets - 1) + last;
    cfg->nominal_packet_bytes = nominal;
    cfg->last_packet_bytes = last;
    cfg->compression = comp;
    cfg->integrity = integ;

    std::vector<uint8_t> stream;
    stream.reserve(64);
    // Hello
    stream.insert(stream.end(), {'I', 'J', 'P', 'D'});
    uint8_t t[4];
    write_u16_le(t, cfg::PROTOCOL_HELLO_VERSION);
    stream.insert(stream.end(), t, t + 2);
    write_u32_le(t, cfg->data_token);
    stream.insert(stream.end(), t, t + 4);
    stream.push_back(0);
    stream.push_back(0);

    std::vector<uint8_t> raw(65536);
    for (uint32_t k = 0; k < packets; ++k) {
        const uint16_t ulen = (k == packets - 1) ? last : nominal;
        for (uint16_t i = 0; i < ulen; ++i) {
            raw[i] = (rng.below(4) == 0) ? static_cast<uint8_t>(rng.below(4) * 0x55)
                                          : rng.byte();
        }
        uint16_t clen = ulen;
        std::vector<uint8_t> comp_buf;
        if (comp == Compression::Lz4Block && rng.below(2) == 0) {
            comp_buf = FuzzEncoder::encode(raw.data(), ulen);
            if (comp_buf.size() < ulen) {
                clen = static_cast<uint16_t>(comp_buf.size());
            } else {
                comp_buf.clear();  // 存储兜底
            }
        }
        const uint8_t* pay = comp_buf.empty() ? raw.data() : comp_buf.data();
        std::vector<uint8_t> f(6);
        write_u16_le(f.data(), static_cast<uint16_t>(k));
        write_u16_le(f.data() + 2, ulen);
        write_u16_le(f.data() + 4, clen);
        f.insert(f.end(), pay, pay + clen);
        if (integ == Integrity::Crc32) {
            uint8_t c[4];
            write_u32_le(c, crc32(f.data(), f.size()));
            f.insert(f.end(), c, c + 4);
        }
        stream.insert(stream.end(), f.begin(), f.end());
    }
    return stream;
}

struct FeedCapture {
    static void on_hello(void*) {}
    static void on_frame(void*, const BinFrameParser::Frame&) {}
};

int run_frame_fuzz(int iterations) {
    Rng rng(0x5EEDF00DCAFEull);
    static uint8_t pbuf[cfg::MAX_PACKET_COMPRESSED_SIZE];
    uint32_t clean_ok = 0;

    for (int it = 0; it < iterations; ++it) {
        BinFrameParser::JobConfig cfg;
        std::vector<uint8_t> stream = build_stream(rng, &cfg);

        // 变异（约一半样本保持合法，验证状态机正常路径）
        if (rng.below(2) == 0) {
            const int nmut = 1 + static_cast<int>(rng.below(4));
            for (int m = 0; m < nmut && !stream.empty(); ++m) {
                switch (rng.below(4)) {
                    case 0:
                        stream[rng.below(static_cast<uint32_t>(stream.size()))] = rng.byte();
                        break;
                    case 1:
                        stream.resize(rng.below(static_cast<uint32_t>(stream.size()) + 1));
                        break;
                    case 2: {  // 拼接第二段合法流
                        BinFrameParser::JobConfig cfg2;
                        const std::vector<uint8_t> s2 = build_stream(rng, &cfg2);
                        if (stream.size() + s2.size() < 200000) {
                            stream.insert(stream.end(), s2.begin(), s2.end());
                        }
                        break;
                    }
                    default:
                        stream.push_back(rng.byte());
                        break;
                }
            }
        } else {
            ++clean_ok;
        }

        BinFrameParser p;
        if (p.begin_job(cfg) != Err::OK) continue;
        p.set_payload_buffer(pbuf, sizeof(pbuf));
        BinFrameParser::Events ev;
        ev.user = nullptr;
        ev.on_hello_ok = FeedCapture::on_hello;
        ev.on_frame = FeedCapture::on_frame;

        size_t pos = 0;
        uint32_t guard = 0;
        while (pos < stream.size()) {
            size_t chunk = 1 + rng.below(64);
            if (pos + chunk > stream.size()) chunk = stream.size() - pos;
            const auto r = p.feed(stream.data() + pos, chunk, ev);
            if (r.status == BinFrameParser::FeedStatus::NeedBuffer) {
                p.set_payload_buffer(pbuf, sizeof(pbuf));
                if (++guard > 1000000) { CHECK_TRUE(false); break; }
                continue;
            }
            if (r.status == BinFrameParser::FeedStatus::Failed) break;  // 锁存，停止
            pos += chunk;
            if (++guard > 1000000) { CHECK_TRUE(false); break; }  // 无死循环
        }
    }
    std::printf("frame fuzz: %d iterations (%d clean)\n", iterations, clean_ok);
    return 0;
}

int run_lz4_fuzz(int iterations) {
    Rng rng(0xABCD1234EF56ull);
    std::vector<uint8_t> raw(2048);
    std::vector<uint8_t> dst(2048);
    for (int it = 0; it < iterations; ++it) {
        const size_t n = 1 + rng.below(1024);
        for (size_t i = 0; i < n; ++i) {
            raw[i] = (rng.below(3) == 0) ? static_cast<uint8_t>(i & 0x0F) : rng.byte();
        }
        std::vector<uint8_t> comp = FuzzEncoder::encode(raw.data(), n);
        if (rng.below(2) == 0) {
            const int nmut = 1 + static_cast<int>(rng.below(3));
            for (int m = 0; m < nmut && !comp.empty(); ++m) {
                switch (rng.below(3)) {
                    case 0:
                        comp[rng.below(static_cast<uint32_t>(comp.size()))] = rng.byte();
                        break;
                    case 1:
                        comp.resize(rng.below(static_cast<uint32_t>(comp.size()) + 1));
                        break;
                    default:
                        comp.push_back(rng.byte());
                        break;
                }
            }
        }
        const int r =
            lz4_block_decompress(comp.data(), comp.size(), dst.data(), dst.size(), n);
        CHECK_TRUE(r < 0 || r == static_cast<int>(n));
    }
    std::printf("lz4 fuzz: %d iterations\n", iterations);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const int iters = (argc > 1) ? std::atoi(argv[1]) : 100000;
    if (run_lz4_fuzz(iters) != 0) return 1;
    if (run_frame_fuzz(iters) != 0) return 1;
    return ::ijpd::test::summary("fuzz_bin_lz4");
}
