// ============================================================================
// json_lite.h —— JSON 子集解析器（就地分词）+ 流式写出器（Phase 1）
//
// 【解析器 JsonParser】
//   - 就地（in-place）解析：对输入缓冲区扫描，字符串转义原地解码（只左移、
//     不扩张），token 记录解码后的偏移与长度；全程零堆分配。
//   - 支持类型：object/array/string/integer/boolean/null；浮点一律 INVALID_JSON。
//   - 约束：token<=JSON_MAX_TOKENS(64)；深度<=JSON_MAX_DEPTH(4)；
//     字符串解码后<=JSON_MAX_STR_BYTES(128)；整数 int64 溢出检查；
//     同对象重复键 -> INVALID_JSON；输入长度 <= 65534（token 偏移为 u16）。
//   - 字符串转义支持 \" \\ \/ \b \f \n \r \t \uXXXX（含合法代理项对）；
//     孤立代理项 / 非法转义 / 裸控制字符(<0x20) / 非法 UTF-8 一律 INVALID_JSON。
//   - 严格 JSON 语法：无尾逗号、无前导零、字面量必须精确匹配。
//   - 类型不匹配的取值（如对 string 调 as_i64）返回 INVALID_ARGUMENT。
//   - “未知字段拒绝”属 schema 层职责（Phase 2）：通过成员遍历接口实现。
//
// 【写出器 JsonWriter】
//   - 流式写入调用方提供的定长缓冲；任何一次写入溢出即置失败态，
//     ok()==false，绝不输出被截断的 JSON（协议要求改发 INTERNAL_ERROR）。
//   - 自动转义 '"' '\\' 控制字符，并把 '#' 强制转义为 \u0023（协议强制）。
//   - 写出器无深度限制（device_info 的 commands 元数据嵌套深于入站限制）。
//
// 线程安全性：两者均非线程安全。JsonParser 每连接一个实例；
//             JsonWriter 每次序列化一个实例（可 reset 复用）。
// 所有权：    解析得到的字符串指向输入缓冲内部，寿命与输入缓冲一致
//             （且仅保证到下次 parse() 前不被破坏）。
// 最大耗时：  解析 O(n)，2048 字节帧 < 1ms @240MHz；写出 O(n)。
// ============================================================================
#ifndef IJPD_CORE_JSON_LITE_H
#define IJPD_CORE_JSON_LITE_H

#include <cstddef>
#include <cstdint>

#include "core/config_limits.h"
#include "core/errors.h"

namespace ijpd {

// ---------------------------------------------------------------------------
class JsonParser {
public:
    static constexpr uint16_t kNone = 0xFFFF;  // 无效 Handle

    enum class Type : uint8_t { Object, Array, String, Integer, Boolean, Null };

    using Handle = uint16_t;

    JsonParser() noexcept = default;
    // 不可拷贝（内嵌 ~1KB token 数组），可移动。
    JsonParser(const JsonParser&) = delete;
    JsonParser& operator=(const JsonParser&) = delete;

    // 就地解析。注意：buf 会被改写（字符串紧凑化解码），len 字节内不会越界。
    // 可重复调用（每次重置内部状态）。
    Err parse(char* buf, size_t len) noexcept;

    // ---- 结构导航 ----
    Handle root() const noexcept { return root_; }
    Type   type(Handle h) const noexcept;
    uint16_t token_count() const noexcept { return count_; }

    // 对象成员（key/value 成对出现在 child 链上）
    Err    member_count(Handle obj, size_t* out) const noexcept;
    Err    member_key(Handle obj, size_t index, const char** out, size_t* len) const noexcept;
    Err    member_value(Handle obj, size_t index, Handle* out) const noexcept;

    // 线性查找（规模小）；未找到返回 kNone（不是错误）。
    Handle find(Handle obj, const char* key) const noexcept;
    bool   str_equals(Handle string_handle, const char* s, size_t s_len) const noexcept;

    // 数组导航（元素按 child 兄弟链顺序）
    Err array_length(Handle arr, size_t* out) const noexcept;
    Err array_element(Handle arr, size_t index, Handle* out) const noexcept;

    // ---- 类型安全取值（类型不符 -> INVALID_ARGUMENT） ----
    Err as_string(Handle h, const char** out, size_t* len) const noexcept;
    Err as_i64(Handle h, int64_t* out) const noexcept;
    Err as_bool(Handle h, bool* out) const noexcept;

    static constexpr size_t kMaxInputBytes = 65534;  // token 偏移为 u16

private:
    struct Token {
        Type type;
        uint8_t _pad0;
        uint16_t parent;  // 父 token（容器），根为 kNone
        uint16_t next;    // 下一个兄弟
        uint16_t child;   // 第一个孩子（仅容器）
        union {
            struct { uint16_t start; uint16_t length; } str;  // 解码后（就地）
            int64_t i64;                                      // Integer / Boolean(0|1)
        } v;
    };
    static_assert(sizeof(Token) == 16, "token layout");

    // 解析辅助（pos 为扫描游标）
    void skip_ws(size_t* pos) const noexcept;
    Err new_token(Type t, uint16_t parent, uint16_t* out) noexcept;
    Err link_child(uint16_t parent, uint16_t child) noexcept;
    bool is_duplicate_key(uint16_t obj, uint16_t new_key) const noexcept;

    Err parse_value(uint32_t depth, size_t* pos, uint16_t parent, uint16_t* out) noexcept;
    Err parse_object(uint32_t depth, size_t* pos, uint16_t tok) noexcept;
    Err parse_array(uint32_t depth, size_t* pos, uint16_t tok) noexcept;
    Err parse_string(size_t* pos, uint16_t parent, uint16_t* out) noexcept;
    Err parse_number(size_t* pos, uint16_t parent, uint16_t* out) noexcept;
    Err parse_literal(size_t* pos, uint16_t parent, uint16_t* out, Type t, bool val,
                      const char* word) noexcept;

    const Token& tok(Handle h) const noexcept { return tokens_[h]; }
    Token& tok(Handle h) noexcept { return tokens_[h]; }
    bool  valid(Handle h) const noexcept { return h != kNone && h < count_; }

    char* buf_ = nullptr;
    size_t len_ = 0;
    uint16_t count_ = 0;
    uint16_t root_ = kNone;
    Token tokens_[cfg::JSON_MAX_TOKENS];
};

// ---------------------------------------------------------------------------
class JsonWriter {
public:
    JsonWriter(char* buf, size_t cap) noexcept : buf_(buf), cap_(cap) {}

    void reset() noexcept;

    bool        ok() const noexcept { return !fail_; }
    size_t      length() const noexcept { return len_; }
    const char* data() const noexcept { return buf_; }

    // 成功时在尾部写入 '\0'（不计入 length()）；无剩余空间则置失败。
    bool terminate() noexcept;

    // 写入控制帧结束符 '#'（响应帧收尾；不计入 JSON 结构）
    bool end_frame() noexcept;

    // ---- 结构 ----
    bool begin_object() noexcept;
    bool begin_array() noexcept;
    bool end_object() noexcept;
    bool end_array() noexcept;
    bool key(const char* k) noexcept;  // 对象内、值之前

    // ---- 值（数组元素 / key 之后的值 / 根标量） ----
    bool value_cstr(const char* s) noexcept;
    bool value_string(const char* s, size_t len) noexcept;
    bool value_i64(int64_t v) noexcept;
    bool value_bool(bool v) noexcept;
    bool value_null() noexcept;

    // ---- 键值快捷 ----
    bool kv_cstr(const char* k, const char* v) noexcept;
    bool kv_string(const char* k, const char* v, size_t len) noexcept;
    bool kv_i64(const char* k, int64_t v) noexcept;
    bool kv_bool(const char* k, bool v) noexcept;
    bool kv_null(const char* k) noexcept;

private:
    static constexpr int kMaxDepth = 16;  // 写出器防误用上限（远大于实际需要）

    struct Frame {
        bool is_object;
        bool first;
    };

    bool append(char c) noexcept;
    bool append(const char* s, size_t n) noexcept;
    bool write_escaped(const char* s, size_t n) noexcept;
    bool write_i64(int64_t v) noexcept;
    bool begin_element_value() noexcept;   // 数组元素/根标量/key 后取值 的逗号与状态机
    bool fail() noexcept;

    char* buf_;
    size_t cap_;
    size_t len_ = 0;
    bool fail_ = false;
    int depth_ = -1;  // -1 = 根
    bool await_value_ = false;
    bool wrote_root_ = false;
    Frame stack_[kMaxDepth] = {};
};

}  // namespace ijpd

#endif  // IJPD_CORE_JSON_LITE_H
