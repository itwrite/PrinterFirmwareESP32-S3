// test_bin_frame_parser.cpp —— Hello 握手 / 帧状态机 / 校验规则 / NeedBuffer
#include "core/bin_frame_parser.h"
#include "assert.h"

#include <cstring>
#include <string>
#include <vector>

#include "core/byte_order.h"
#include "core/crc32.h"

using namespace ijpd;

namespace {

struct Capture {
    std::vector<std::pair<uint16_t, std::vector<uint8_t>>> frames;  // (index, payload)
    bool hello_ok = false;
    uint8_t* auto_buffer = nullptr;  // on_frame 内自动重设缓冲（模拟 rx_task）

    static void on_hello(void* user) { static_cast<Capture*>(user)->hello_ok = true; }
    static void on_frame(void* user, const BinFrameParser::Frame& f) {
        auto* self = static_cast<Capture*>(user);
        self->frames.emplace_back(f.index,
                                  std::vector<uint8_t>(f.payload, f.payload + f.compressed_len));
        if (self->auto_buffer != nullptr) {
            // 回调内重设缓冲（粘包场景的自然位置）
        }
    }
    static BinFrameParser::Events events(Capture* c) {
        BinFrameParser::Events ev;
        ev.user = c;
        ev.on_hello_ok = on_hello;
        ev.on_frame = on_frame;
        return ev;
    }
};

std::vector<uint8_t> hello_bytes(uint32_t token) {
    std::vector<uint8_t> b{'I', 'J', 'P', 'D'};
    uint8_t t[2];
    write_u16_le(t, cfg::PROTOCOL_HELLO_VERSION);
    b.insert(b.end(), t, t + 2);
    uint8_t t4[4];
    write_u32_le(t4, token);
    b.insert(b.end(), t4, t4 + 4);
    b.push_back(0);
    b.push_back(0);
    return b;
}

// 构造一帧（不校验合法性；ulen/clen 原样写入）
std::vector<uint8_t> frame_bytes(uint16_t idx, uint16_t ulen, uint16_t clen,
                                 const uint8_t* payload, bool with_crc) {
    std::vector<uint8_t> b(6);
    write_u16_le(b.data(), idx);
    write_u16_le(b.data() + 2, ulen);
    write_u16_le(b.data() + 4, clen);
    b.insert(b.end(), payload, payload + clen);
    if (with_crc) {
        uint8_t c[4];
        write_u32_le(c, crc32(b.data(), b.size()));
        b.insert(b.end(), c, c + 4);
    }
    return b;
}

BinFrameParser::JobConfig make_cfg(Compression comp, Integrity integ, uint32_t packets,
                                   uint16_t nominal, uint16_t last, uint32_t token) {
    BinFrameParser::JobConfig cfg{};
    cfg.data_token = token;
    cfg.total_packets = packets;
    cfg.total_bytes_uncompressed =
        static_cast<uint64_t>(nominal) * (packets - 1) + last;
    cfg.nominal_packet_bytes = nominal;
    cfg.last_packet_bytes = last;
    cfg.compression = comp;
    cfg.integrity = integ;
    return cfg;
}

std::vector<uint8_t> payload_of(char c, size_t n) {
    return std::vector<uint8_t>(n, static_cast<uint8_t>(c));
}

}  // namespace

int main() {
    static uint8_t pbuf[2][cfg::MAX_PACKET_COMPRESSED_SIZE];  // 乒乓缓冲

    // ================= 1. 完整正常流（none 模式，无 CRC，单包任务） =================
    {
        BinFrameParser p;
        Capture cap;
        CHECK_TRUE(p.begin_job(make_cfg(Compression::None, Integrity::None, 1, 8, 8,
                                        0xDEADBEEF)) == Err::OK);
        p.set_payload_buffer(pbuf[0], sizeof(pbuf[0]));

        const std::vector<uint8_t> pay = payload_of('A', 8);
        std::vector<uint8_t> stream = hello_bytes(0xDEADBEEF);
        const std::vector<uint8_t> f = frame_bytes(0, 8, 8, pay.data(), false);
        stream.insert(stream.end(), f.begin(), f.end());

        const auto r = p.feed(stream.data(), stream.size(), Capture::events(&cap));
        CHECK_TRUE(r.status == BinFrameParser::FeedStatus::Ok);
        CHECK_TRUE(cap.hello_ok);
        CHECK_EQ(r.frames, 1u);
        CHECK_EQ(cap.frames.size(), 1u);
        CHECK_EQ(cap.frames[0].first, 0);
        CHECK_EQ(cap.frames[0].second.size(), 8u);
        CHECK_EQ_MEM(cap.frames[0].second.data(), pay.data(), 8);
        CHECK_TRUE(p.phase() == BinFrameParser::Phase::Done);
        CHECK_EQ(p.packets_received(), 1u);
        CHECK_EQ(p.bytes_received(), 8u);
    }

    // ================= 2. Hello 分字节投喂 + 校验失败 =================
    {
        BinFrameParser p;
        Capture cap;
        p.begin_job(make_cfg(Compression::None, Integrity::None, 1, 4, 4, 7));
        p.set_payload_buffer(pbuf[0], sizeof(pbuf[0]));
        const auto h = hello_bytes(7);
        for (size_t i = 0; i < h.size(); ++i) {
            const auto r = p.feed(h.data() + i, 1, Capture::events(&cap));
            CHECK_TRUE(r.status == BinFrameParser::FeedStatus::Ok);
        }
        CHECK_TRUE(cap.hello_ok);
        CHECK_TRUE(p.phase() == BinFrameParser::Phase::Header);
    }
    {
        struct Bad { std::vector<uint8_t> hello; };
        std::vector<uint8_t> bad1{'X', 'J', 'P', 'D', 1, 0, 7, 0, 0, 0, 0, 0};   // 魔数错
        std::vector<uint8_t> bad2 = hello_bytes(7);
        bad2[4] = 2;                                                             // 版本错
        std::vector<uint8_t> bad3 = hello_bytes(99);                             // 令牌错
        std::vector<uint8_t> bad4 = hello_bytes(7);
        bad4[10] = 1;                                                            // 保留字段非 0
        const std::vector<std::vector<uint8_t>> bads = {bad1, bad2, bad3, bad4};
        for (const auto& h : bads) {
            BinFrameParser p;
            Capture cap;
            p.begin_job(make_cfg(Compression::None, Integrity::None, 1, 4, 4, 7));
            p.set_payload_buffer(pbuf[0], sizeof(pbuf[0]));
            const auto r = p.feed(h.data(), h.size(), Capture::events(&cap));
            CHECK_TRUE(r.status == BinFrameParser::FeedStatus::Failed);
            CHECK_TRUE(r.error == Err::DATA_HANDSHAKE_FAILED);
            CHECK_TRUE(!cap.hello_ok);
            // 失败锁存
            const auto r2 = p.feed(h.data(), h.size(), Capture::events(&cap));
            CHECK_TRUE(r2.status == BinFrameParser::FeedStatus::Failed);
            CHECK_TRUE(r2.error == Err::DATA_HANDSHAKE_FAILED);
        }
    }
    {
        // 未配置任务直接 feed
        BinFrameParser p;
        Capture cap;
        const uint8_t x = 0;
        const auto r = p.feed(&x, 1, Capture::events(&cap));
        CHECK_TRUE(r.status == BinFrameParser::FeedStatus::Failed);
        CHECK_TRUE(r.error == Err::NO_ACTIVE_JOB);
    }

    // ================= 3. 多帧粘包 + 随机切块 + CRC =================
    {
        BinFrameParser p;
        Capture cap;
        const uint16_t nominal = 16, last = 9;
        const uint32_t packets = 4;
        CHECK_TRUE(p.begin_job(make_cfg(Compression::None, Integrity::Crc32, packets,
                                        nominal, last, 42)) == Err::OK);
        p.set_payload_buffer(pbuf[0], sizeof(pbuf[0]));

        std::vector<uint8_t> stream = hello_bytes(42);
        for (uint32_t k = 0; k < packets; ++k) {
            const uint16_t ulen = (k == packets - 1) ? last : nominal;
            const std::vector<uint8_t> pay = payload_of(static_cast<char>('a' + k), ulen);
            const auto f = frame_bytes(static_cast<uint16_t>(k), ulen, ulen, pay.data(), true);
            stream.insert(stream.end(), f.begin(), f.end());
        }

        // 随机切块（固定种子）
        uint64_t seed = 0xBEEF;
        auto rnd = [&seed]() {
            seed ^= seed >> 12; seed ^= seed << 25; seed ^= seed >> 27;
            return seed * 2685821657736338717ull;
        };
        size_t pos = 0;
        uint32_t total_frames = 0;
        while (pos < stream.size()) {
            size_t chunk = static_cast<size_t>(rnd() % 13) + 1;
            if (pos + chunk > stream.size()) chunk = stream.size() - pos;
            auto r = p.feed(stream.data() + pos, chunk, Capture::events(&cap));
            CHECK_TRUE(r.consumed <= chunk);
            pos += r.consumed;  // 无论 Ok/NeedBuffer 都按 consumed 推进
            if (r.status == BinFrameParser::FeedStatus::NeedBuffer) {
                p.set_payload_buffer(pbuf[total_frames % 2], sizeof(pbuf[0]));
                continue;  // 从 consumed 之后重喂
            }
            CHECK_TRUE(r.status == BinFrameParser::FeedStatus::Ok);
            total_frames += r.frames;
        }
        CHECK_EQ(total_frames, packets);
        CHECK_EQ(cap.frames.size(), static_cast<size_t>(packets));
        for (uint32_t k = 0; k < packets; ++k) {
            CHECK_EQ(cap.frames[k].first, k);
            const uint16_t ulen = (k == packets - 1) ? last : nominal;
            CHECK_EQ(cap.frames[k].second.size(), ulen);
            CHECK_EQ(cap.frames[k].second[0], 'a' + k);
        }
        CHECK_TRUE(p.phase() == BinFrameParser::Phase::Done);
        CHECK_EQ(p.bytes_received(),
                 static_cast<uint64_t>(nominal) * (packets - 1) + last);
    }

    // ================= 4. NeedBuffer 精确语义 =================
    {
        // 帧 0 完成 + 帧 1 完整头部在同一块；缓冲未设 -> NeedBuffer，consumed 定位
        BinFrameParser p;
        Capture cap;
        CHECK_TRUE(p.begin_job(make_cfg(Compression::None, Integrity::None, 2, 8, 8, 5)) == Err::OK);
        p.set_payload_buffer(pbuf[0], sizeof(pbuf[0]));

        const auto pay = payload_of('z', 8);
        std::vector<uint8_t> stream = hello_bytes(5);
        auto f0 = frame_bytes(0, 8, 8, pay.data(), false);
        stream.insert(stream.end(), f0.begin(), f0.end());
        const auto f1 = frame_bytes(1, 8, 8, pay.data(), false);
        stream.insert(stream.end(), f1.begin(), f1.begin() + 6);  // 只有帧 1 的头

        auto r = p.feed(stream.data(), stream.size(), Capture::events(&cap));
        CHECK_TRUE(r.status == BinFrameParser::FeedStatus::Ok);
        CHECK_EQ(r.frames, 1u);

        // 现在喂帧 1 的载荷，但未重设缓冲（派发后 buf 被清空）
        const std::vector<uint8_t> rest(f1.begin() + 6, f1.end());
        r = p.feed(rest.data(), rest.size(), Capture::events(&cap));
        CHECK_TRUE(r.status == BinFrameParser::FeedStatus::NeedBuffer);
        CHECK_EQ(r.consumed, 0u);
        // 设缓冲后重喂
        p.set_payload_buffer(pbuf[1], sizeof(pbuf[1]));
        r = p.feed(rest.data(), rest.size(), Capture::events(&cap));
        CHECK_TRUE(r.status == BinFrameParser::FeedStatus::Ok);
        CHECK_EQ(r.frames, 1u);
        CHECK_EQ(cap.frames.size(), 2u);
        CHECK_TRUE(p.phase() == BinFrameParser::Phase::Done);
    }
    {
        // 头部恰好在本块用尽完成且无载荷字节：返回 Ok（不强制 NeedBuffer）
        BinFrameParser p;
        Capture cap;
        CHECK_TRUE(p.begin_job(make_cfg(Compression::None, Integrity::None, 2, 8, 8, 5)) == Err::OK);
        p.set_payload_buffer(pbuf[0], sizeof(pbuf[0]));
        const auto pay = payload_of('q', 8);
        std::vector<uint8_t> stream = hello_bytes(5);
        const auto f0 = frame_bytes(0, 8, 8, pay.data(), false);
        stream.insert(stream.end(), f0.begin(), f0.end());
        const auto f1 = frame_bytes(1, 8, 8, pay.data(), false);
        stream.insert(stream.end(), f1.begin(), f1.begin() + 6);
        auto r = p.feed(stream.data(), stream.size(), Capture::events(&cap));
        CHECK_TRUE(r.status == BinFrameParser::FeedStatus::Ok);
        CHECK_TRUE(p.phase() == BinFrameParser::Phase::Payload);
        CHECK_EQ(p.packets_received(), 1u);
        p.set_payload_buffer(pbuf[1], sizeof(pbuf[1]));
        const std::vector<uint8_t> body(f1.begin() + 6, f1.end());
        r = p.feed(body.data(), body.size(), Capture::events(&cap));
        CHECK_TRUE(r.status == BinFrameParser::FeedStatus::Ok);
        CHECK_EQ(r.frames, 1u);
    }

    // ================= 5. 校验规则矩阵 =================
    {
        struct Case {
            uint16_t idx, ulen, clen;
            Err want;
            const char* name;
        };
        // 任务：3 包，nominal=8，last=5，none 模式，无 CRC（每行独立新解析器）
        const std::vector<Case> cases = {
            {1, 8, 8, Err::PACKET_OUT_OF_ORDER, "乱序"},
            {0, 8, 8, Err::OK, "合法首包"},
            {2, 8, 8, Err::PACKET_OUT_OF_ORDER, "跳包"},
            {0, 0, 0, Err::PACKET_LENGTH_INVALID, "零长度"},
            {0, 7, 7, Err::PACKET_LENGTH_INVALID, "非末包短包"},
            {0, 8, 7, Err::PACKET_LENGTH_INVALID, "none: clen!=ulen"},
            {0, 8, 9, Err::PACKET_LENGTH_INVALID, "none: clen>ulen"},
        };
        for (const auto& c : cases) {
            BinFrameParser p;
            Capture cap;
            CHECK_TRUE(p.begin_job(make_cfg(Compression::None, Integrity::None, 3, 8, 5, 1)) ==
                       Err::OK);
            p.set_payload_buffer(pbuf[0], sizeof(pbuf[0]));
            const auto h = hello_bytes(1);
            const auto r0 = p.feed(h.data(), h.size(), Capture::events(&cap));
            CHECK_TRUE(r0.status == BinFrameParser::FeedStatus::Ok);

            const std::vector<uint8_t> pay = payload_of('p', c.clen != 0 ? c.clen : 1);
            const auto f = frame_bytes(c.idx, c.ulen, c.clen, pay.data(), false);
            const auto r = p.feed(f.data(), f.size(), Capture::events(&cap));
            if (c.want == Err::OK) {
                CHECK_TRUE(r.status == BinFrameParser::FeedStatus::Ok);
            } else {
                CHECK_TRUE(r.status == BinFrameParser::FeedStatus::Failed);
                CHECK_TRUE(r.error == c.want);
            }
        }
    }
    {
        // 序列：重复序号（第 2 个同号包 -> PACKET_OUT_OF_ORDER）
        BinFrameParser p;
        Capture cap;
        CHECK_TRUE(p.begin_job(make_cfg(Compression::None, Integrity::None, 3, 8, 5, 1)) ==
                   Err::OK);
        p.set_payload_buffer(pbuf[0], sizeof(pbuf[0]));
        p.feed(hello_bytes(1).data(), 12, Capture::events(&cap));
        const auto pay = payload_of('d', 8);
        const auto f0 = frame_bytes(0, 8, 8, pay.data(), false);
        CHECK_TRUE(p.feed(f0.data(), f0.size(), Capture::events(&cap)).status ==
                   BinFrameParser::FeedStatus::Ok);
        p.set_payload_buffer(pbuf[1], sizeof(pbuf[1]));
        const auto r = p.feed(f0.data(), f0.size(), Capture::events(&cap));
        CHECK_TRUE(r.status == BinFrameParser::FeedStatus::Failed);
        CHECK_TRUE(r.error == Err::PACKET_OUT_OF_ORDER);
    }
    {
        // 序列：末包长度错误（前两包合法，末包 ulen=8 应为 5）
        BinFrameParser p;
        Capture cap;
        CHECK_TRUE(p.begin_job(make_cfg(Compression::None, Integrity::None, 3, 8, 5, 1)) ==
                   Err::OK);
        p.set_payload_buffer(pbuf[0], sizeof(pbuf[0]));
        p.feed(hello_bytes(1).data(), 12, Capture::events(&cap));
        const auto pay = payload_of('e', 8);
        for (int k = 0; k < 2; ++k) {
            const auto f = frame_bytes(static_cast<uint16_t>(k), 8, 8, pay.data(), false);
            CHECK_TRUE(p.feed(f.data(), f.size(), Capture::events(&cap)).status ==
                       BinFrameParser::FeedStatus::Ok);
            p.set_payload_buffer(pbuf[k % 2], sizeof(pbuf[0]));
        }
        const auto flast = frame_bytes(2, 8, 8, pay.data(), false);
        const auto r = p.feed(flast.data(), flast.size(), Capture::events(&cap));
        CHECK_TRUE(r.status == BinFrameParser::FeedStatus::Failed);
        CHECK_TRUE(r.error == Err::PACKET_LENGTH_INVALID);
    }
    {
        // lz4 模式：clen==ulen（存储兜底）合法；clen<ulen 合法；clen>ulen 非法
        struct Case { uint16_t ulen, clen; Err want; };
        const std::vector<Case> cases = {
            {8, 8, Err::OK},
            {8, 4, Err::OK},
            {8, 9, Err::PACKET_LENGTH_INVALID},
        };
        for (const auto& c : cases) {
            BinFrameParser p;
            Capture cap;
            CHECK_TRUE(p.begin_job(make_cfg(Compression::Lz4Block, Integrity::None, 2, 8, 8, 1)) ==
                       Err::OK);
            p.set_payload_buffer(pbuf[0], sizeof(pbuf[0]));
            p.feed(hello_bytes(1).data(), 12, Capture::events(&cap));
            const std::vector<uint8_t> pay = payload_of('l', c.clen);
            const auto f = frame_bytes(0, c.ulen, c.clen, pay.data(), false);
            const auto r = p.feed(f.data(), f.size(), Capture::events(&cap));
            if (c.want == Err::OK) {
                CHECK_TRUE(r.status == BinFrameParser::FeedStatus::Ok);
            } else {
                CHECK_TRUE(r.status == BinFrameParser::FeedStatus::Failed);
                CHECK_TRUE(r.error == c.want);
            }
        }
    }

    // ================= 6. CRC 单比特翻转 =================
    {
        BinFrameParser p;
        Capture cap;
        CHECK_TRUE(p.begin_job(make_cfg(Compression::None, Integrity::Crc32, 2, 8, 8, 9)) ==
                   Err::OK);
        p.set_payload_buffer(pbuf[0], sizeof(pbuf[0]));
        p.feed(hello_bytes(9).data(), 12, Capture::events(&cap));
        const auto pay = payload_of('c', 8);
        auto f = frame_bytes(0, 8, 8, pay.data(), true);
        // 翻转载荷区一个比特（偏移 8，位于头部之后）
        f[8] = static_cast<uint8_t>(f[8] ^ 0x01);
        const auto r = p.feed(f.data(), f.size(), Capture::events(&cap));
        CHECK_TRUE(r.status == BinFrameParser::FeedStatus::Failed);
        CHECK_TRUE(r.error == Err::PACKET_CRC_MISMATCH);

        // 翻转 CRC 自身
        BinFrameParser p2;
        Capture cap2;
        p2.begin_job(make_cfg(Compression::None, Integrity::Crc32, 2, 8, 8, 9));
        p2.set_payload_buffer(pbuf[0], sizeof(pbuf[0]));
        p2.feed(hello_bytes(9).data(), 12, Capture::events(&cap2));
        auto f2 = frame_bytes(0, 8, 8, pay.data(), true);
        f2[f2.size() - 1] = static_cast<uint8_t>(f2[f2.size() - 1] ^ 0x80);
        const auto r2 = p2.feed(f2.data(), f2.size(), Capture::events(&cap2));
        CHECK_TRUE(r2.status == BinFrameParser::FeedStatus::Failed);
        CHECK_TRUE(r2.error == Err::PACKET_CRC_MISMATCH);
    }

    // ================= 7. 末包后多余字节 / 逐字节投喂极端半包 =================
    {
        BinFrameParser p;
        Capture cap;
        p.begin_job(make_cfg(Compression::None, Integrity::None, 1, 4, 4, 3));
        p.set_payload_buffer(pbuf[0], sizeof(pbuf[0]));
        const auto pay = payload_of('e', 4);
        std::vector<uint8_t> stream = hello_bytes(3);
        const auto f = frame_bytes(0, 4, 4, pay.data(), false);
        stream.insert(stream.end(), f.begin(), f.end());
        stream.push_back(0xAA);  // 末包后多余
        const auto r = p.feed(stream.data(), stream.size(), Capture::events(&cap));
        CHECK_TRUE(r.status == BinFrameParser::FeedStatus::Failed);
        CHECK_TRUE(r.error == Err::PACKET_LENGTH_INVALID);
    }
    {
        // 全流逐字节
        BinFrameParser p;
        Capture cap;
        const uint16_t nominal = 6, last = 3;
        p.begin_job(make_cfg(Compression::Lz4Block, Integrity::Crc32, 3, nominal, last, 11));
        p.set_payload_buffer(pbuf[0], sizeof(pbuf[0]));
        std::vector<uint8_t> stream = hello_bytes(11);
        for (uint32_t k = 0; k < 3; ++k) {
            const uint16_t ulen = (k == 2) ? last : nominal;
            const auto pay = payload_of(static_cast<char>('0' + k), ulen);
            const auto f = frame_bytes(static_cast<uint16_t>(k), ulen,
                                       static_cast<uint16_t>(ulen - 1), pay.data(), true);
            stream.insert(stream.end(), f.begin(), f.end());
        }
        // 注意：clen=ulen-1 时载荷只有 clen 字节参与 CRC —— frame_bytes 已按 clen 构造
        size_t pos = 0;
        uint32_t got = 0;
        while (pos < stream.size()) {
            const auto r = p.feed(stream.data() + pos, 1, Capture::events(&cap));
            if (r.status == BinFrameParser::FeedStatus::NeedBuffer) {
                p.set_payload_buffer(pbuf[got % 2], sizeof(pbuf[0]));
                continue;
            }
            CHECK_TRUE(r.status == BinFrameParser::FeedStatus::Ok);
            got += r.frames;
            ++pos;
        }
        CHECK_EQ(got, 3u);
        CHECK_EQ(cap.frames.size(), 3u);
        CHECK_EQ(cap.frames[2].second.size(), 2u);  // clen = last-1
        CHECK_TRUE(p.phase() == BinFrameParser::Phase::Done);
    }

    // ================= 8. begin_job 配置校验 / reset =================
    {
        BinFrameParser p;
        auto bad1 = make_cfg(Compression::None, Integrity::None, 0, 8, 8, 1);   // 0 包
        auto bad2 = make_cfg(Compression::None, Integrity::None, 3, 0, 8, 1);   // nominal 0
        auto bad3 = make_cfg(Compression::None, Integrity::None, 3, 8, 9, 1);   // last>nominal
        auto bad4 = make_cfg(Compression::None, Integrity::None, 3, 8, 5, 1);   // 总量不符
        bad4.total_bytes_uncompressed = 999;
        CHECK_TRUE(p.begin_job(bad1) == Err::INVALID_ARGUMENT);
        CHECK_TRUE(p.begin_job(bad2) == Err::INVALID_ARGUMENT);
        CHECK_TRUE(p.begin_job(bad3) == Err::INVALID_ARGUMENT);
        CHECK_TRUE(p.begin_job(bad4) == Err::INVALID_ARGUMENT);
        CHECK_TRUE(p.phase() == BinFrameParser::Phase::Idle);

        // set_payload_buffer 容量校验
        CHECK_TRUE(p.set_payload_buffer(pbuf[0], 100) == Err::INVALID_ARGUMENT);
        CHECK_TRUE(p.set_payload_buffer(pbuf[0], cfg::MAX_PACKET_COMPRESSED_SIZE) == Err::OK);

        // reset 回 Idle
        CHECK_TRUE(p.begin_job(make_cfg(Compression::None, Integrity::None, 1, 8, 8, 1)) ==
                   Err::OK);
        p.reset();
        CHECK_TRUE(p.phase() == BinFrameParser::Phase::Idle);
        const uint8_t x = 1;
        CHECK_TRUE(p.feed(&x, 1, Capture::events(nullptr)).status ==
                   BinFrameParser::FeedStatus::Failed);
    }

    return ::ijpd::test::summary("test_bin_frame_parser");
}
