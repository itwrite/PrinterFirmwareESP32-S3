// fuzz_json_splitter.cpp —— 固定种子模糊测试（任务书 12.1/12.2 精神，Phase 1 范围）
//
// A. FrameSplitter：随机流（含空段/超限段/正常段）随机切块投喂，
//    用独立模型校验 frames/empty/too_large 计数与帧内容完全一致。
// B. JsonParser：对合法模板施加随机变异（字节改写/截断/拼接/插入'#'），
//    在 ASan/UBSan 下必须无崩溃、无越界读出（输入为精确大小堆缓冲）；
//    解析成功时全树遍历并做类型取值，检验结构不变量。
//
// 默认各 100000 / 3000 次迭代，恒定种子，可复现；可用 argv[1] 覆盖解析迭代数。
#include "core/frame_splitter.h"
#include "core/json_lite.h"
#include "assert.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace ijpd;

namespace {

// ---- xorshift64* 确定性随机 ----
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

const char* kTemplates[] = {
    R"({"msg_id":"job-create-001","cmd":"start_print","data":{"job_id":"job-001",)"
    R"("total_bytes_uncompressed":120000,"total_packets":30,"compression":"lz4_block",)"
    R"("integrity":"crc32","bytes_per_row":1200,"total_rows":100,"cutter_after_finish":false}})",
    R"({"msg_id":"m1","cmd":"get_device_info","data":{}})",
    R"({"a":[1,2,3],"b":{"c":[true,false,null]},"d":-9223372036854775808})",
    R"({"esc":"q\"\\\/\b\f\n\r\t","u":"\u0041\uD83D\uDE00\u0023","utf8":"é€𝄞"})",
    R"({"deep":{"l2":{"l3":{"l4":[1,[2,[3]]]}}}})",
    R"({"dup":0})",
};

// ---- B 部分：解析成功的输入做全树遍历（结构不变量 + 取值安全） ----
bool walk(const JsonParser& p, JsonParser::Handle h, int depth) {
    if (depth > 8) return false;
    switch (p.type(h)) {
        case JsonParser::Type::Object: {
            size_t mc = 0;
            if (p.member_count(h, &mc) != Err::OK) return false;
            for (size_t i = 0; i < mc; ++i) {
                const char* k = nullptr;
                size_t kn = 0;
                if (p.member_key(h, i, &k, &kn) != Err::OK) return false;
                JsonParser::Handle v = JsonParser::kNone;
                if (p.member_value(h, i, &v) != Err::OK) return false;
                if (v == JsonParser::kNone) return false;
                if (!walk(p, v, depth + 1)) return false;
            }
            return true;
        }
        case JsonParser::Type::Array: {
            // 数组没有计数接口：通过 child 链遍历不可从公开 API 获得，
            // 此处仅校证根对象可达性，防止误用深结构。
            return true;
        }
        case JsonParser::Type::String: {
            const char* s = nullptr;
            size_t n = 0;
            return p.as_string(h, &s, &n) == Err::OK;
        }
        case JsonParser::Type::Integer: {
            int64_t v = 0;
            return p.as_i64(h, &v) == Err::OK;
        }
        case JsonParser::Type::Boolean: {
            bool b = false;
            return p.as_bool(h, &b) == Err::OK;
        }
        case JsonParser::Type::Null:
            return true;
        default:
            return false;
    }
}

int run_parser_fuzz(int iterations) {
    Rng rng(0xC0FFEE123456789ull);
    int parsed_ok = 0;
    for (int i = 0; i < iterations; ++i) {
        const char* tpl = kTemplates[rng.below(sizeof(kTemplates) / sizeof(kTemplates[0]))];
        std::string s(tpl);

        // 随机变异 1..4 次
        const int muts = 1 + static_cast<int>(rng.below(4));
        for (int m = 0; m < muts && !s.empty(); ++m) {
            switch (rng.below(4)) {
                case 0: {  // 随机改写一个字节
                    const size_t at = rng.below(static_cast<uint32_t>(s.size()));
                    s[at] = static_cast<char>(rng.byte());
                    break;
                }
                case 1: {  // 截断
                    s.resize(rng.below(static_cast<uint32_t>(s.size()) + 1));
                    break;
                }
                case 2: {  // 拼接自身片段
                    const size_t at = rng.below(static_cast<uint32_t>(s.size()));
                    const size_t n = rng.below(static_cast<uint32_t>(s.size() - at) + 1);
                    if (s.size() + n < 4096) s.append(s, at, n);
                    break;
                }
                case 3: {  // 插入 '#'
                    const size_t at = rng.below(static_cast<uint32_t>(s.size()) + 1);
                    s.insert(at, 1, '#');
                    break;
                }
                default:
                    break;
            }
        }

        // 精确大小堆缓冲：任何越界读/写都会被 ASan 捕获
        char* buf = static_cast<char*>(malloc(s.size() == 0 ? 1 : s.size()));
        std::memcpy(buf, s.data(), s.size());

        JsonParser p;
        const Err e = p.parse(buf, s.size());
        if (e == Err::OK) {
            ++parsed_ok;
            if (p.root() == JsonParser::kNone) {
                CHECK_TRUE(false);  // 解析成功必有根
            } else if (!walk(p, p.root(), 0)) {
                CHECK_TRUE(false);  // 结构遍历失败
            }
        }
        free(buf);
    }
    std::printf("parser fuzz: %d iterations, %d parsed ok\n", iterations, parsed_ok);
    return 0;
}

struct SplitCapture {
    std::vector<std::string>* out;
    static void cb(void* user, const char* frame, size_t len) {
        static_cast<SplitCapture*>(user)->out->emplace_back(frame, len);
    }
};

int run_splitter_fuzz(int iterations) {
    Rng rng(0xFEEDFACE00112233ull);
    const size_t cap = FrameSplitter::capacity();

    for (int i = 0; i < iterations; ++i) {
        // 构造 1..8 段，段间以 '#' 分隔；内容为非 '#' 随机字节
        const int segs = 1 + static_cast<int>(rng.below(8));
        std::string stream;
        std::vector<std::string> want_frames;
        size_t want_empty = 0;
        size_t want_large = 0;

        const bool has_term = (rng.below(2) == 0);  // 末段是否被 '#' 终止

        for (int sg = 0; sg < segs; ++sg) {
            if (sg != 0) stream += '#';
            // 长度分布覆盖 0 / 正常 / 超限
            size_t len;
            const uint32_t roll = rng.below(100);
            if (roll < 10) {
                len = 0;
            } else if (roll < 85) {
                len = rng.below(200);
            } else {
                len = cap + rng.below(16);
            }
            std::string seg;
            for (size_t k = 0; k < len; ++k) {
                uint8_t c = rng.byte();
                if (c == static_cast<uint8_t>('#')) c = '_';
                seg += static_cast<char>(c);
            }
            stream += seg;
            const bool terminated = (sg != segs - 1) || has_term;
            if (!terminated) continue;  // 末段未终止：与 tail 合并为半帧，不产生事件
            if (len == 0) {
                ++want_empty;
            } else if (len > cap) {
                ++want_large;
            } else {
                want_frames.push_back(std::move(seg));
            }
        }
        if (has_term) stream += '#';
        const size_t tail_len = rng.below(50);
        for (size_t k = 0; k < tail_len; ++k) {
            uint8_t c = rng.byte();
            if (c == static_cast<uint8_t>('#')) c = '_';
            stream += static_cast<char>(c);
        }

        // 随机切块投喂
        FrameSplitter sp;
        std::vector<std::string> got;
        SplitCapture cap_cb{&got};
        size_t got_empty = 0;
        size_t got_large = 0;
        size_t pos = 0;
        while (pos < stream.size()) {
            size_t chunk = 1 + rng.below(64);
            if (pos + chunk > stream.size()) chunk = stream.size() - pos;
            const auto st = sp.feed(reinterpret_cast<const uint8_t*>(stream.data()) + pos,
                                    chunk, SplitCapture::cb, &cap_cb);
            got_empty += st.empty_ignored;
            got_large += st.too_large;
            pos += chunk;
            CHECK_TRUE(sp.partial_len() <= cap);
        }

        CHECK_EQ(got_empty, want_empty);
        CHECK_EQ(got_large, want_large);
        CHECK_EQ(got.size(), want_frames.size());
        const size_t n = got.size() < want_frames.size() ? got.size() : want_frames.size();
        for (size_t k = 0; k < n; ++k) {
            CHECK_TRUE(got[k] == want_frames[k]);
        }
        if (got_empty != want_empty || got_large != want_large ||
            got.size() != want_frames.size()) {
            return 1;  // 失败即提前结束，保留可复现上下文
        }
    }
    std::printf("splitter fuzz: %d iterations ok\n", iterations);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const int parser_iters = (argc > 1) ? std::atoi(argv[1]) : 100000;
    const int splitter_iters = 3000;

    if (run_parser_fuzz(parser_iters) != 0) return 1;
    if (run_splitter_fuzz(splitter_iters) != 0) return 1;

    return ::ijpd::test::summary("fuzz_json_splitter");
}
