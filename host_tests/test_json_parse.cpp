// test_json_parse.cpp —— json_lite 解析器：类型/转义/UTF-8/约束/导航
#include "core/json_lite.h"
#include "assert.h"

#include <cstdlib>
#include <cstring>
#include <string>

using namespace ijpd;

namespace {

// RAII 持有精确大小的堆缓冲（parse 会就地改写并保留内部指针，
// 缓冲必须存活到全部断言结束；精确大小使 ASan 可捕获任何越界）。
class Parsed {
public:
    JsonParser p;

    explicit Parsed(const char* s) { init(s, std::strlen(s)); }
    explicit Parsed(const std::string& s) { init(s.data(), s.size()); }
    ~Parsed() { std::free(buf_); }

    Parsed(const Parsed&) = delete;
    Parsed& operator=(const Parsed&) = delete;

    bool ok() const { return err_ == Err::OK; }
    Err err() const { return err_; }

private:
    void init(const char* s, size_t n) {
        buf_ = static_cast<char*>(std::malloc(n != 0 ? n : 1));
        std::memcpy(buf_, s, n);
        err_ = p.parse(buf_, n);
    }
    char* buf_ = nullptr;
    Err err_ = Err::INTERNAL_ERROR;
};

Err err_of(const std::string& s) {
    Parsed pd(s);
    return pd.err();
}

}  // namespace

int main() {
    using Type = JsonParser::Type;

    // ================= 基础结构 =================
    {
        Parsed pd(R"({"msg_id":"m1","cmd":"get_device_info","data":{}})");
        const JsonParser& p = pd.p;
        CHECK_TRUE(pd.ok());
        CHECK_TRUE(p.type(p.root()) == Type::Object);
        size_t mc = 99;
        CHECK_TRUE(ok(p.member_count(p.root(), &mc)));
        CHECK_EQ(mc, 3);
        CHECK_TRUE(p.str_equals(p.find(p.root(), "msg_id"), "m1", 2));
        CHECK_TRUE(p.str_equals(p.find(p.root(), "cmd"), "get_device_info", 15));
        const JsonParser::Handle data = p.find(p.root(), "data");
        CHECK_TRUE(data != JsonParser::kNone);
        CHECK_TRUE(p.type(data) == Type::Object);
        CHECK_TRUE(p.find(p.root(), "nope") == JsonParser::kNone);
    }

    // 完整 start_print 请求示例（protocol_v1.md §4.3）
    {
        const char* req =
            R"({"msg_id":"job-create-001","cmd":"start_print","data":{)"
            R"("job_id":"job-001","total_bytes_uncompressed":120000,"total_packets":30,)"
            R"("compression":"lz4_block","integrity":"crc32","bytes_per_row":1200,)"
            R"("total_rows":100,"cutter_after_finish":false}})";
        Parsed pd(req);
        const JsonParser& p = pd.p;
        CHECK_TRUE(pd.ok());
        const JsonParser::Handle data = p.find(p.root(), "data");
        int64_t v = 0;
        CHECK_TRUE(ok(p.as_i64(p.find(data, "total_bytes_uncompressed"), &v)));
        CHECK_EQ(v, 120000);
        CHECK_TRUE(ok(p.as_i64(p.find(data, "total_packets"), &v)));
        CHECK_EQ(v, 30);
        bool b = true;
        CHECK_TRUE(ok(p.as_bool(p.find(data, "cutter_after_finish"), &b)));
        CHECK_EQ(b ? 1 : 0, 0);
        CHECK_TRUE(p.str_equals(p.find(data, "compression"), "lz4_block", 9));
    }

    // ================= 整数 =================
    {
        struct Case { const char* json; int64_t want; };
        const Case cases[] = {
            {R"({"v":0})", 0},
            {R"({"v":-0})", 0},
            {R"({"v":42})", 42},
            {R"({"v":-42})", -42},
            {R"({"v":9223372036854775807})", INT64_MAX},
            {R"({"v":-9223372036854775808})", INT64_MIN},
        };
        for (const auto& c : cases) {
            Parsed pd(c.json);
            CHECK_TRUE(pd.ok());
            int64_t v = 0;
            CHECK_TRUE(ok(pd.p.as_i64(pd.p.find(pd.p.root(), "v"), &v)));
            CHECK_EQ(v, c.want);
        }
    }
    {
        const char* bad[] = {
            R"({"v":9223372036854775808})",   // int64 溢出
            R"({"v":-9223372036854775809})",  // int64 溢出
            R"({"v":01})",                    // 前导零
            R"({"v":-})",                     // '-' 后无数字
            R"({"v":1.5})",                   // 浮点不支持
            R"({"v":1e3})",                   // 浮点不支持
            R"({"v":1E3})",
            R"({"v":123abc})",                // 粘尾
            R"({"v":+1})",                    // 非法起始
        };
        for (const char* j : bad) {
            CHECK_TRUE(err_of(j) == Err::INVALID_JSON);
        }
    }

    // ================= 布尔 / null / 数组 / 根标量 =================
    {
        Parsed pd(R"({"t":true,"f":false,"n":null,"a":[1,[2,3],[]],"s":"x"})");
        const JsonParser& p = pd.p;
        CHECK_TRUE(pd.ok());
        bool bv = false;
        CHECK_TRUE(ok(p.as_bool(p.find(p.root(), "t"), &bv)) && bv);
        CHECK_TRUE(ok(p.as_bool(p.find(p.root(), "f"), &bv)) && !bv);
        CHECK_TRUE(p.type(p.find(p.root(), "n")) == Type::Null);
        CHECK_TRUE(p.type(p.find(p.root(), "a")) == Type::Array);
        CHECK_TRUE(p.type(p.find(p.root(), "s")) == Type::String);
    }
    {
        Parsed p1("123");
        CHECK_TRUE(p1.ok() && p1.p.type(p1.p.root()) == Type::Integer);
        Parsed p2(R"("hello")");
        CHECK_TRUE(p2.ok() && p2.p.type(p2.p.root()) == Type::String);
    }
    {
        const char* bad[] = {
            R"({"v":tru})", R"({"v":trux})", R"({"v":nullx})", R"({"v":fals})",
            R"([1,])", R"({"a":1,})", R"({"a" 1})", R"({a:1})", R"({'a':1})",
            R"({"a":1}x)", R"( )", R"([1 2])", R"({)", R"([)",
            R"({"a":)", R"("unterminated)", R"(\)",
        };
        for (const char* j : bad) {
            CHECK_TRUE(err_of(j) == Err::INVALID_JSON);
        }
        Parsed pEmpty("");
        CHECK_TRUE(pEmpty.err() == Err::INVALID_JSON);
    }

    // ================= 转义与 \uXXXX =================
    {
        Parsed pd(R"({"s":"q\"\\\/\b\f\n\r\tZ"})");
        const JsonParser& p = pd.p;
        CHECK_TRUE(pd.ok());
        const char* s = nullptr;
        size_t n = 0;
        CHECK_TRUE(ok(p.as_string(p.find(p.root(), "s"), &s, &n)));
        const char want[] = {'q', '"', '\\', '/', '\b', '\f', '\n', '\r', '\t', 'Z'};
        CHECK_EQ(n, sizeof(want));
        CHECK_EQ_MEM(s, want, sizeof(want));
    }
    {
        Parsed pd(R"({"a":"\u0041"})");
        const char* s = nullptr;
        size_t n = 0;
        CHECK_TRUE(pd.ok());
        CHECK_TRUE(ok(pd.p.as_string(pd.p.find(pd.p.root(), "a"), &s, &n)));
        CHECK_EQ(n, 1u);
        CHECK_EQ(s[0], 'A');
    }
    {
        Parsed pd(R"({"a":"\u0023"})");  // 协议 '#' 转义通道
        const char* s = nullptr;
        size_t n = 0;
        CHECK_TRUE(pd.ok());
        CHECK_TRUE(ok(pd.p.as_string(pd.p.find(pd.p.root(), "a"), &s, &n)));
        CHECK_EQ(n, 1u);
        CHECK_EQ(s[0], '#');
    }
    {
        // 合法代理项对 U+1F600 -> F0 9F 98 80
        Parsed pd(R"({"e":"\uD83D\uDE00"})");
        const char* s = nullptr;
        size_t n = 0;
        CHECK_TRUE(pd.ok());
        CHECK_TRUE(ok(pd.p.as_string(pd.p.find(pd.p.root(), "e"), &s, &n)));
        CHECK_EQ(n, 4u);
        const uint8_t want[] = {0xF0, 0x9F, 0x98, 0x80};
        CHECK_EQ_MEM(s, want, 4);
    }
    {
        const char* bad[] = {
            R"({"a":"\uD800"})",          // 孤立高代理项
            R"({"a":"\uDC00"})",          // 孤立低代理项
            R"({"a":"\uD800A"})",         // 高代理项后非转义
            R"({"a":"\uD800\u0041"})",    // 高代理项 + 非低代理项
            R"({"a":"\x41"})",            // 非法转义
            R"({"a":"\u12"})",            // \u 不足 4 位
            R"({"a":"\uZZZZ"})",          // 非十六进制
            "{\"a\":\"raw\nnewline\"}",   // 裸控制字符（换行）
            "{\"a\":\"raw\ttab\"}",       // 裸控制字符（制表符）
        };
        for (const char* j : bad) {
            CHECK_TRUE(err_of(j) == Err::INVALID_JSON);
        }
    }

    // ================= UTF-8 校验 =================
    {
        const uint8_t valid[] = {'a', 0xC3, 0xA9, 0xE2, 0x82, 0xAC,
                                 0xF0, 0x9D, 0x84, 0x9E, 'b'};  // a é € 𝄞 b
        std::string json = "{\"s\":\"";
        json.append(reinterpret_cast<const char*>(valid), sizeof(valid));
        json += "\"}";
        Parsed pd(json);
        CHECK_TRUE(pd.ok());
        const char* s = nullptr;
        size_t n = 0;
        CHECK_TRUE(ok(pd.p.as_string(pd.p.find(pd.p.root(), "s"), &s, &n)));
        CHECK_EQ(n, sizeof(valid));
        CHECK_EQ_MEM(s, valid, sizeof(valid));
    }
    {
        const uint8_t b1[] = {0xC0, 0x80};                 // 超长 C0 80
        const uint8_t b2[] = {0xC1, 0xBF};                 // 超长 C1
        const uint8_t b3[] = {0xE0, 0x80, 0x80};           // 超长 3 字节
        const uint8_t b4[] = {0xED, 0xA0, 0x80};           // 代理项 D800 的 UTF-8 编码
        const uint8_t b5[] = {0xF5, 0x80, 0x80, 0x80};     // > U+10FFFF
        const uint8_t b6[] = {0xF0, 0x8F, 0xBF, 0xBF};     // 超长 4 字节
        const uint8_t b7[] = {0xC3, 0x28};                 // 断续
        const uint8_t b8[] = {0xC3};                       // 截断
        const uint8_t b9[] = {0x80};                       // 孤立续字节
        const uint8_t b10[] = {0xE2, 0x82};                // 3 字节截断
        const struct { const uint8_t* b; size_t n; } bads[] = {
            {b1, sizeof(b1)}, {b2, sizeof(b2)}, {b3, sizeof(b3)}, {b4, sizeof(b4)},
            {b5, sizeof(b5)}, {b6, sizeof(b6)}, {b7, sizeof(b7)}, {b8, sizeof(b8)},
            {b9, sizeof(b9)}, {b10, sizeof(b10)},
        };
        for (const auto& bd : bads) {
            std::string json = "{\"s\":\"";
            json.append(reinterpret_cast<const char*>(bd.b), bd.n);
            json += "\"}";
            Parsed pd(json);
            CHECK_TRUE(pd.err() == Err::INVALID_JSON);
        }
    }

    // ================= 重复键 / 深度 / token 上限 / 字符串长度 =================
    {
        CHECK_TRUE(err_of(R"({"a":1,"a":2})") == Err::INVALID_JSON);
        CHECK_TRUE(err_of(R"({"a":{"b":{"c":{"d":{"e":1}}}}})") == Err::INVALID_JSON);  // 深度 5
        Parsed pd4(R"({"a":{"b":{"c":{"d":1}}}})");  // 深度 4 合法
        CHECK_TRUE(pd4.ok());
        CHECK_TRUE(err_of("[[[[[]]]]]") == Err::INVALID_JSON);  // 数组深度 5
        Parsed pdA("[[[[]]]]");  // 数组深度 4 合法
        CHECK_TRUE(pdA.ok());
    }
    {
        // token 上限 64：50 键值对 = 1 + 100 token -> 超限
        std::string big = "{";
        for (int i = 0; i < 50; ++i) {
            if (i != 0) big += ',';
            big += "\"k" + std::to_string(i) + "\":" + std::to_string(i);
        }
        big += "}";
        CHECK_TRUE(err_of(big) == Err::INVALID_JSON);

        // 20 键值对 = 41 token -> 合法
        std::string ok_json = "{";
        for (int i = 0; i < 20; ++i) {
            if (i != 0) ok_json += ',';
            ok_json += "\"k" + std::to_string(i) + "\":" + std::to_string(i);
        }
        ok_json += "}";
        Parsed pd(ok_json);
        CHECK_TRUE(pd.ok());
    }
    {
        // 字符串解码后 <= 128 字节
        Parsed pd128(std::string("{") + "\"s\":\"" + std::string(128, 'a') + "\"}");
        CHECK_TRUE(pd128.ok());
        Parsed pd129(std::string("{") + "\"s\":\"" + std::string(129, 'a') + "\"}");
        CHECK_TRUE(pd129.err() == Err::INVALID_JSON);

        // 转义膨胀：126 × \u0041 -> 126 字节（合法）；129 × -> 129 字节（非法）
        std::string esc126;
        for (int i = 0; i < 126; ++i) esc126 += "\\u0041";
        Parsed pde(std::string("{") + "\"s\":\"" + esc126 + "\"}");
        CHECK_TRUE(pde.ok());
        const char* s = nullptr;
        size_t sn = 0;
        CHECK_TRUE(ok(pde.p.as_string(pde.p.find(pde.p.root(), "s"), &s, &sn)));
        CHECK_EQ(sn, 126u);

        std::string esc129;
        for (int i = 0; i < 129; ++i) esc129 += "\\u0041";
        Parsed pdbad(std::string("{") + "\"s\":\"" + esc129 + "\"}");
        CHECK_TRUE(pdbad.err() == Err::INVALID_JSON);
    }

    // ================= 前后空白 / 解析失败后复用 =================
    {
        Parsed pdWs(" \t\r\n{\"a\":1} \r\n");
        CHECK_TRUE(pdWs.ok());
        CHECK_TRUE(err_of("{\"a\":1} {\"b\":2}") == Err::INVALID_JSON);
        // 失败后重新 parse 复用（同一解析器、同一缓冲重新填充）
        char buf[32];
        std::memcpy(buf, "{bad", 4);
        JsonParser p;
        CHECK_TRUE(p.parse(buf, 4) == Err::INVALID_JSON);
        std::memcpy(buf, R"({"a":"ok"})", 10);
        CHECK_TRUE(p.parse(buf, 10) == Err::OK);
        CHECK_TRUE(p.str_equals(p.find(p.root(), "a"), "ok", 2));
    }

    // ================= 成员遍历（schema 严格模式将用到） =================
    {
        Parsed pd(R"({"b":2,"a":1,"c":3})");
        const JsonParser& p = pd.p;
        CHECK_TRUE(pd.ok());
        size_t mc = 0;
        CHECK_TRUE(ok(p.member_count(p.root(), &mc)));
        CHECK_EQ(mc, 3);
        const char* k = nullptr;
        size_t kn = 0;
        CHECK_TRUE(ok(p.member_key(p.root(), 0, &k, &kn)));
        CHECK_EQ_MEM(k, "b", 1);
        JsonParser::Handle v = JsonParser::kNone;
        CHECK_TRUE(ok(p.member_value(p.root(), 0, &v)));
        int64_t iv = 0;
        CHECK_TRUE(ok(p.as_i64(v, &iv)));
        CHECK_EQ(iv, 2);
        CHECK_TRUE(ok(p.member_key(p.root(), 2, &k, &kn)));
        CHECK_EQ_MEM(k, "c", 1);
        CHECK_TRUE(p.member_value(p.root(), 3, &v) == Err::INVALID_ARGUMENT);   // 越界
        CHECK_TRUE(p.member_count(v, &mc) == Err::INVALID_ARGUMENT);            // 非对象
    }

    // ================= 类型不匹配取值 =================
    {
        Parsed pd(R"({"s":"str","i":7,"b":true,"n":null})");
        const JsonParser& p = pd.p;
        CHECK_TRUE(pd.ok());
        int64_t iv = 0;
        const char* s = nullptr;
        size_t sn = 0;
        bool bv = false;
        CHECK_TRUE(p.as_i64(p.find(p.root(), "s"), &iv) == Err::INVALID_ARGUMENT);
        CHECK_TRUE(p.as_bool(p.find(p.root(), "i"), &bv) == Err::INVALID_ARGUMENT);
        CHECK_TRUE(p.as_string(p.find(p.root(), "i"), &s, &sn) == Err::INVALID_ARGUMENT);
        CHECK_TRUE(p.as_bool(p.find(p.root(), "n"), &bv) == Err::INVALID_ARGUMENT);
        CHECK_TRUE(p.as_i64(JsonParser::kNone, &iv) == Err::INVALID_ARGUMENT);
    }

    // ================= 嵌套数组/对象导航 =================
    {
        Parsed pd(R"({"data":{"rows":[[1,2,3],[4,5,6]],"meta":{"dpi":300,"w":1800}}})");
        const JsonParser& p = pd.p;
        CHECK_TRUE(pd.ok());
        const JsonParser::Handle data = p.find(p.root(), "data");
        const JsonParser::Handle rows = p.find(data, "rows");
        CHECK_TRUE(p.type(rows) == Type::Array);
        const JsonParser::Handle meta = p.find(data, "meta");
        int64_t dpi = 0;
        CHECK_TRUE(ok(p.as_i64(p.find(meta, "dpi"), &dpi)));
        CHECK_EQ(dpi, 300);
    }

    return ::ijpd::test::summary("test_json_parse");
}
