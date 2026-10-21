// test_frame_splitter.cpp —— '#' 拆帧：正常/半包/粘包/空帧/超限/超时辅助
#include "core/frame_splitter.h"
#include "assert.h"

#include <cstring>
#include <vector>

using namespace ijpd;

namespace {

struct Capture {
    std::vector<std::string> frames;
    static void cb(void* user, const char* frame, size_t len) {
        auto* self = static_cast<Capture*>(user);
        self->frames.emplace_back(frame, len);
    }
};

}  // namespace

int main() {
    // ---- 1. 单帧一次到达 ----
    {
        FrameSplitter s;
        Capture c;
        const char* in = R"({"cmd":"x"})";
        auto st = s.feed(reinterpret_cast<const uint8_t*>(in), std::strlen(in),
                         Capture::cb, &c);
        CHECK_EQ(st.frames, 0);  // 尚无 '#'
        CHECK_TRUE(s.has_partial());
        CHECK_EQ(s.partial_len(), std::strlen(in));

        const uint8_t hash = '#';
        st = s.feed(&hash, 1, Capture::cb, &c);
        CHECK_EQ(st.frames, 1);
        CHECK_EQ(st.empty_ignored, 0);
        CHECK_EQ(st.too_large, 0);
        CHECK_EQ(c.frames.size(), 1u);
        CHECK_EQ_STR(c.frames[0].c_str(), R"({"cmd":"x"})");
        CHECK_TRUE(!s.has_partial());
    }

    // ---- 2. 逐字节投喂（极端半包） ----
    {
        FrameSplitter s;
        Capture c;
        const char* in = "abcdef#";
        for (size_t i = 0; i < std::strlen(in); ++i) {
            const uint8_t ch = static_cast<uint8_t>(in[i]);
            s.feed(&ch, 1, Capture::cb, &c);
        }
        CHECK_EQ(c.frames.size(), 1u);
        CHECK_EQ_STR(c.frames[0].c_str(), "abcdef");
    }

    // ---- 3. 一次到达多条帧 + 空帧 ----
    {
        FrameSplitter s;
        Capture c;
        const char* in = "a#bb##ccc#";
        const auto st = s.feed(reinterpret_cast<const uint8_t*>(in), std::strlen(in),
                               Capture::cb, &c);
        CHECK_EQ(st.frames, 3);
        CHECK_EQ(st.empty_ignored, 1);
        CHECK_EQ(st.too_large, 0);
        CHECK_EQ(c.frames.size(), 3u);
        CHECK_EQ_STR(c.frames[0].c_str(), "a");
        CHECK_EQ_STR(c.frames[1].c_str(), "bb");
        CHECK_EQ_STR(c.frames[2].c_str(), "ccc");
    }

    // ---- 4. 帧长 == 上限：合法 ----
    {
        FrameSplitter s;
        Capture c;
        std::vector<uint8_t> in(FrameSplitter::capacity() + 1, 'x');
        in[FrameSplitter::capacity()] = '#';
        const auto st = s.feed(in.data(), in.size(), Capture::cb, &c);
        CHECK_EQ(st.frames, 1);
        CHECK_EQ(st.too_large, 0);
        CHECK_EQ(c.frames.size(), 1u);
        CHECK_EQ(c.frames[0].size(), FrameSplitter::capacity());
    }

    // ---- 5. 帧长 = 上限+1：FRAME_TOO_LARGE，缓冲不增长 ----
    {
        FrameSplitter s;
        Capture c;
        std::vector<uint8_t> in(FrameSplitter::capacity() + 2, 'y');
        in[FrameSplitter::capacity() + 1] = '#';
        const auto st = s.feed(in.data(), in.size(), Capture::cb, &c);
        CHECK_EQ(st.frames, 0);
        CHECK_EQ(st.too_large, 1);
        CHECK_EQ(c.frames.size(), 0u);
        CHECK_TRUE(!s.has_partial());
    }

    // ---- 6. 超限分多次投喂，且后续正常帧不受影响 ----
    {
        FrameSplitter s;
        Capture c;
        const size_t cap = FrameSplitter::capacity();
        std::vector<uint8_t> junk(cap + 10, 'z');
        for (size_t i = 0; i < junk.size(); ++i) {
            const uint8_t ch = junk[i];
            s.feed(&ch, 1, Capture::cb, &c);
            CHECK_TRUE(s.partial_len() <= cap);  // 不变量：绝不超上限
        }
        const char* rest = "#good#tail";
        s.feed(reinterpret_cast<const uint8_t*>(rest), std::strlen(rest), Capture::cb, &c);
        CHECK_EQ(c.frames.size(), 1u);
        CHECK_EQ_STR(c.frames[0].c_str(), "good");
        CHECK_TRUE(s.has_partial());
        CHECK_EQ_STR(std::string("tail").c_str(), "tail");  // 剩余半帧
        CHECK_EQ(s.partial_len(), 4u);
    }

    // ---- 7. clear_partial（超时丢弃半帧） ----
    {
        FrameSplitter s;
        Capture c;
        const char* in = "half_frame_no_hash";
        s.feed(reinterpret_cast<const uint8_t*>(in), std::strlen(in), Capture::cb, &c);
        CHECK_TRUE(s.has_partial());
        s.clear_partial();
        CHECK_TRUE(!s.has_partial());
        CHECK_EQ(s.partial_len(), 0u);
        // 清除后新帧从零开始
        const char* next = "ok#";
        const auto st = s.feed(reinterpret_cast<const uint8_t*>(next), std::strlen(next),
                               Capture::cb, &c);
        CHECK_EQ(st.frames, 1);
        CHECK_EQ_STR(c.frames[0].c_str(), "ok");
    }

    // ---- 8. 连续 '#' 与起始 '#' ----
    {
        FrameSplitter s;
        Capture c;
        const char* in = "##a##";
        const auto st = s.feed(reinterpret_cast<const uint8_t*>(in), std::strlen(in),
                               Capture::cb, &c);
        CHECK_EQ(st.frames, 1);
        CHECK_EQ(st.empty_ignored, 3);
        CHECK_EQ_STR(c.frames[0].c_str(), "a");
    }

    // ---- 9. 帧内含 '\u0023' 转义（无裸 '#'，字节级安全） ----
    {
        FrameSplitter s;
        Capture c;
        const char* in = R"({"job_id":"job-\u00231"}#)";
        const auto st = s.feed(reinterpret_cast<const uint8_t*>(in), std::strlen(in),
                               Capture::cb, &c);
        CHECK_EQ(st.frames, 1);
        CHECK_EQ(c.frames.size(), 1u);
        CHECK_EQ(c.frames[0].size(), std::strlen(R"({"job_id":"job-\u00231"})"));
    }

    // ---- 10. 多次 too_large 逐帧计数 ----
    {
        FrameSplitter s;
        Capture c;
        std::string in;
        in.append(FrameSplitter::capacity() + 1, 'a');
        in += '#';
        in.append(FrameSplitter::capacity() + 5, 'b');
        in += '#';
        in += "fine#";
        const auto st = s.feed(reinterpret_cast<const uint8_t*>(in.data()), in.size(),
                               Capture::cb, &c);
        CHECK_EQ(st.too_large, 2);
        CHECK_EQ(st.frames, 1);
        CHECK_EQ_STR(c.frames[0].c_str(), "fine");
    }

    // ---- 11. 随机切块（固定种子）投喂多帧流 ----
    {
        uint64_t seed = 0x123456789ABCDEF0ull;
        auto rnd = [&seed]() {
            seed ^= seed >> 12; seed ^= seed << 25; seed ^= seed >> 27;
            return seed * 2685821657736338717ull;
        };
        const std::string stream = "f1#f22#f333##f4444#partial";
        FrameSplitter s;
        Capture c;
        size_t pos = 0;
        while (pos < stream.size()) {
            size_t chunk = static_cast<size_t>(rnd() % 7) + 1;
            if (pos + chunk > stream.size()) chunk = stream.size() - pos;
            s.feed(reinterpret_cast<const uint8_t*>(stream.data()) + pos, chunk,
                   Capture::cb, &c);
            pos += chunk;
        }
        CHECK_EQ(c.frames.size(), 4u);
        CHECK_EQ_STR(c.frames[0].c_str(), "f1");
        CHECK_EQ_STR(c.frames[1].c_str(), "f22");
        CHECK_EQ_STR(c.frames[2].c_str(), "f333");
        CHECK_EQ_STR(c.frames[3].c_str(), "f4444");
        CHECK_TRUE(s.has_partial());
        CHECK_EQ(s.partial_len(), 7u);  // "partial"
    }

    return ::ijpd::test::summary("test_frame_splitter");
}
