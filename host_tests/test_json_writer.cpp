// test_json_writer.cpp —— 流式写出器：结构/转义/整数/容量
#include "core/json_lite.h"
#include "assert.h"

#include <cstring>
#include <string>

using namespace ijpd;

int main() {
    char buf[512];

    // ---- 1. 基本对象与嵌套 ----
    {
        JsonWriter w(buf, sizeof(buf));
        CHECK_TRUE(w.begin_object());
        CHECK_TRUE(w.kv_cstr("msg_id", "m1"));
        CHECK_TRUE(w.kv_cstr("cmd", "get_device_info"));
        CHECK_TRUE(w.key("data"));
        CHECK_TRUE(w.begin_object());
        CHECK_TRUE(w.kv_bool("success", true));
        CHECK_TRUE(w.kv_null("error"));
        CHECK_TRUE(w.end_object());
        CHECK_TRUE(w.end_object());
        CHECK_TRUE(w.ok());
        CHECK_EQ_STR(std::string(w.data(), w.length()).c_str(),
                     R"({"msg_id":"m1","cmd":"get_device_info","data":{"success":true,"error":null}})");
    }

    // ---- 2. 数组、空容器、整数 ----
    {
        JsonWriter w(buf, sizeof(buf));
        CHECK_TRUE(w.begin_object());
        CHECK_TRUE(w.key("a"));
        CHECK_TRUE(w.begin_array());
        CHECK_TRUE(w.value_i64(1));
        CHECK_TRUE(w.value_i64(-2));
        CHECK_TRUE(w.value_i64(0));
        CHECK_TRUE(w.end_array());
        CHECK_TRUE(w.kv_i64("empty_arr_idx", 0));
        CHECK_TRUE(w.key("e"));
        CHECK_TRUE(w.begin_array());
        CHECK_TRUE(w.end_array());
        CHECK_TRUE(w.key("o"));
        CHECK_TRUE(w.begin_object());
        CHECK_TRUE(w.end_object());
        CHECK_TRUE(w.key("bounds"));
        CHECK_TRUE(w.begin_array());
        CHECK_TRUE(w.value_i64(INT64_MAX));
        CHECK_TRUE(w.value_i64(INT64_MIN));
        CHECK_TRUE(w.end_array());
        CHECK_TRUE(w.end_object());
        CHECK_TRUE(w.ok());
        CHECK_EQ_STR(std::string(w.data(), w.length()).c_str(),
                     R"({"a":[1,-2,0],"empty_arr_idx":0,"e":[],"o":{},"bounds":[9223372036854775807,-9223372036854775808]})");
    }

    // ---- 3. 转义：'#' 强制 \u0023，引号/反斜杠/控制字符 ----
    {
        JsonWriter w(buf, sizeof(buf));
        CHECK_TRUE(w.begin_object());
        CHECK_TRUE(w.kv_cstr("hash", "a#b"));
        CHECK_TRUE(w.kv_cstr("quote", "say\"hi\""));
        CHECK_TRUE(w.kv_cstr("back", "C:\\dir"));
        CHECK_TRUE(w.kv_string("ctrl", "a\nb\tc", 5));
        CHECK_TRUE(w.end_object());
        CHECK_TRUE(w.ok());
        CHECK_EQ_STR(std::string(w.data(), w.length()).c_str(),
                     R"({"hash":"a\u0023b","quote":"say\"hi\"","back":"C:\\dir","ctrl":"a\nb\tc"})");
    }

    // ---- 4. 键转义同样生效 ----
    {
        JsonWriter w(buf, sizeof(buf));
        CHECK_TRUE(w.begin_object());
        CHECK_TRUE(w.key("k#1"));
        CHECK_TRUE(w.value_i64(5));
        CHECK_TRUE(w.end_object());
        CHECK_TRUE(w.ok());
        CHECK_EQ_STR(std::string(w.data(), w.length()).c_str(),
                     R"({"k\u00231":5})");
    }

    // ---- 5. 容量恰好足够：成功 ----
    {
        const std::string want = R"({"a":1,"b":2})";  // 13 字节
        char small[13];
        JsonWriter w(small, sizeof(small));
        CHECK_TRUE(w.begin_object());
        CHECK_TRUE(w.kv_i64("a", 1));
        CHECK_TRUE(w.kv_i64("b", 2));
        CHECK_TRUE(w.end_object());
        CHECK_TRUE(w.ok());
        CHECK_EQ(w.length(), want.size());
        CHECK_EQ_MEM(w.data(), want.data(), want.size());
        // terminate 无剩余空间（len==cap）——按契约置失败（无 NUL 空间）
        CHECK_TRUE(!w.terminate());
        CHECK_TRUE(!w.ok());
    }

    // ---- 6. 容量不足 1 字节：整条失败，绝不出截断 JSON ----
    // {"a":1,"b":2 为 12 字节（恰好占满），闭合 '}' 需要第 13 字节 -> 溢出。
    {
        char small[12];
        JsonWriter w(small, sizeof(small));
        CHECK_TRUE(w.begin_object());
        CHECK_TRUE(w.kv_i64("a", 1));
        CHECK_TRUE(w.kv_i64("b", 2));    // 恰好 12 字节，尚可写入
        CHECK_TRUE(w.ok());
        CHECK_TRUE(!w.end_object());     // 第 13 字节溢出 -> 整条失败（粘滞）
        CHECK_TRUE(!w.ok());
        CHECK_TRUE(!w.kv_i64("c", 3));   // 失败态下所有调用均失败
        CHECK_TRUE(w.length() <= sizeof(small));  // 不越界
    }

    // ---- 7. 溢出后长写入不越界（大字符串压满） ----
    {
        char small[16];
        JsonWriter w(small, sizeof(small));
        CHECK_TRUE(w.begin_object());
        std::string big(64, 'x');
        CHECK_TRUE(!w.kv_string("k", big.data(), big.size()));
        CHECK_TRUE(!w.ok());
    }

    // ---- 8. 误用检测（失败态粘滞：任一误用即整条作废） ----
    {
        // 8a. 对象内取值必须先 key()
        JsonWriter w(buf, sizeof(buf));
        CHECK_TRUE(w.begin_object());
        CHECK_TRUE(!w.value_i64(1));
        CHECK_TRUE(!w.ok());            // 粘滞
        CHECK_TRUE(!w.key("a"));
        CHECK_TRUE(!w.end_object());
    }
    {
        // 8b. key 后必须先值
        JsonWriter w(buf, sizeof(buf));
        CHECK_TRUE(w.begin_object());
        CHECK_TRUE(w.key("a"));
        CHECK_TRUE(!w.key("b"));
        CHECK_TRUE(!w.value_i64(1));    // 粘滞
        CHECK_TRUE(!w.end_object());
        CHECK_TRUE(!w.ok());
    }
    {
        // 8c. end 与容器类型错配
        JsonWriter w(buf, sizeof(buf));
        CHECK_TRUE(w.begin_object());
        CHECK_TRUE(w.kv_i64("a", 1));
        CHECK_TRUE(!w.end_array());
        CHECK_TRUE(!w.end_object());    // 粘滞
        CHECK_TRUE(!w.ok());
    }
    {
        // 8d. 正常路径对照（额外 end_object 属误用 -> 粘滞）
        JsonWriter w(buf, sizeof(buf));
        CHECK_TRUE(w.begin_object());
        CHECK_TRUE(w.kv_i64("a", 1));
        CHECK_TRUE(w.end_object());
        CHECK_EQ_STR(std::string(w.data(), w.length()).c_str(), R"({"a":1})");
        CHECK_TRUE(!w.end_object());    // 已到根：误用
        CHECK_TRUE(!w.ok());            // 粘滞
    }

    // ---- 9. 根标量与数组根 ----
    {
        JsonWriter w(buf, sizeof(buf));
        CHECK_TRUE(w.value_i64(42));
        CHECK_TRUE(w.ok());
        CHECK_EQ_STR(std::string(w.data(), w.length()).c_str(), "42");
        CHECK_TRUE(!w.value_i64(1));  // 根只能一个值

        JsonWriter w2(buf, sizeof(buf));
        CHECK_TRUE(w2.begin_array());
        CHECK_TRUE(w2.value_cstr("x"));
        CHECK_TRUE(w2.value_bool(false));
        CHECK_TRUE(w2.value_null());
        CHECK_TRUE(w2.end_array());
        CHECK_TRUE(w2.ok());
        CHECK_EQ_STR(std::string(w2.data(), w2.length()).c_str(), R"(["x",false,null])");
    }

    // ---- 10. reset 复用 ----
    {
        JsonWriter w(buf, sizeof(buf));
        CHECK_TRUE(w.begin_object());
        CHECK_TRUE(w.kv_i64("a", 1));
        CHECK_TRUE(w.end_object());
        w.reset();
        CHECK_TRUE(w.begin_object());
        CHECK_TRUE(w.kv_i64("b", 2));
        CHECK_TRUE(w.end_object());
        CHECK_TRUE(w.ok());
        CHECK_EQ_STR(std::string(w.data(), w.length()).c_str(), R"({"b":2})");
    }

    // ---- 11. terminate 正常路径 ----
    {
        JsonWriter w(buf, sizeof(buf));
        CHECK_TRUE(w.begin_object());
        CHECK_TRUE(w.kv_i64("a", 1));
        CHECK_TRUE(w.end_object());
        CHECK_TRUE(w.terminate());
        CHECK_TRUE(w.ok());
        CHECK_EQ_STR(w.data(), R"({"a":1})");
        CHECK_EQ(w.length(), 7u);
    }

    // ---- 12. 大响应骨架（device_info 量级）分块无碍 ----
    {
        char big[2048];
        JsonWriter w(big, sizeof(big));
        CHECK_TRUE(w.begin_object());
        CHECK_TRUE(w.key("commands"));
        CHECK_TRUE(w.begin_array());
        for (int i = 0; i < 8; ++i) {
            CHECK_TRUE(w.begin_object());
            CHECK_TRUE(w.kv_cstr("label", "cmd"));
            CHECK_TRUE(w.key("params"));
            CHECK_TRUE(w.begin_array());
            CHECK_TRUE(w.begin_object());
            CHECK_TRUE(w.kv_cstr("key", "total_bytes_uncompressed"));
            CHECK_TRUE(w.kv_cstr("type", "integer"));
            CHECK_TRUE(w.kv_bool("required", true));
            CHECK_TRUE(w.kv_i64("min", 1));
            CHECK_TRUE(w.kv_i64("max", 67108864));
            CHECK_TRUE(w.end_object());
            CHECK_TRUE(w.end_array());
            CHECK_TRUE(w.end_object());
        }
        CHECK_TRUE(w.end_array());
        CHECK_TRUE(w.end_object());
        CHECK_TRUE(w.ok());
        CHECK_TRUE(w.length() > 300u);
    }

    return ::ijpd::test::summary("test_json_writer");
}
