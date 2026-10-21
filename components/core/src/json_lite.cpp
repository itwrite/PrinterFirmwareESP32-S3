// json_lite.cpp —— JSON 子集解析 + 流式写出实现
#include "core/json_lite.h"

#include <cstring>

namespace ijpd {
namespace {

inline bool is_ws(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

inline bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

inline bool is_value_delim(char c) noexcept {
    return is_ws(c) || c == ',' || c == '}' || c == ']';
}

inline size_t cstr_len(const char* s) noexcept { return std::strlen(s); }

// 读取 4 个十六进制字符（\uXXXX 的 XXXX），成功后 *pos 前进 4
Err read_hex4(const char* buf, size_t len, size_t* pos, uint32_t* out) noexcept {
    if (*pos + 4 > len) return Err::INVALID_JSON;
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        const char c = buf[*pos + static_cast<size_t>(i)];
        uint32_t d;
        if (c >= '0' && c <= '9') {
            d = static_cast<uint32_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = static_cast<uint32_t>(c - 'a') + 10;
        } else if (c >= 'A' && c <= 'F') {
            d = static_cast<uint32_t>(c - 'A') + 10;
        } else {
            return Err::INVALID_JSON;
        }
        v = (v << 4) | d;
    }
    *pos += 4;
    *out = v;
    return Err::OK;
}

inline bool is_cont(unsigned char c) noexcept { return (c & 0xC0u) == 0x80u; }

// 严格 UTF-8 校验（拒绝：非法引导字节、断续、超长编码、代理项编码、>U+10FFFF）
bool utf8_valid(const char* p, size_t avail, size_t* n) noexcept {
    const auto b0 = static_cast<unsigned char>(p[0]);
    const auto b1 = (avail >= 2) ? static_cast<unsigned char>(p[1]) : 0u;
    const auto b2 = (avail >= 3) ? static_cast<unsigned char>(p[2]) : 0u;
    const auto b3 = (avail >= 4) ? static_cast<unsigned char>(p[3]) : 0u;

    if (b0 < 0x80) { *n = 1; return true; }
    if (b0 >= 0xC2 && b0 <= 0xDF) {
        if (avail < 2 || !is_cont(b1)) return false;
        *n = 2; return true;
    }
    if (b0 == 0xE0) {
        if (avail < 3 || b1 < 0xA0 || b1 > 0xBF || !is_cont(b2)) return false;
        *n = 3; return true;
    }
    if ((b0 >= 0xE1 && b0 <= 0xEC) || b0 == 0xEE || b0 == 0xEF) {
        if (avail < 3 || !is_cont(b1) || !is_cont(b2)) return false;
        *n = 3; return true;
    }
    if (b0 == 0xED) {  // 排除代理项 D800-DFFF 的 UTF-8 编码（ED A0..BF）
        if (avail < 3 || b1 < 0x80 || b1 > 0x9F || !is_cont(b2)) return false;
        *n = 3; return true;
    }
    if (b0 == 0xF0) {  // 排除超长（F0 80..8F）
        if (avail < 4 || b1 < 0x90 || b1 > 0xBF || !is_cont(b2) || !is_cont(b3)) return false;
        *n = 4; return true;
    }
    if (b0 >= 0xF1 && b0 <= 0xF3) {
        if (avail < 4 || !is_cont(b1) || !is_cont(b2) || !is_cont(b3)) return false;
        *n = 4; return true;
    }
    if (b0 == 0xF4) {  // 上界 U+10FFFF（F4 90+ 非法）
        if (avail < 4 || b1 < 0x80 || b1 > 0x8F || !is_cont(b2) || !is_cont(b3)) return false;
        *n = 4; return true;
    }
    return false;  // 0x80-0xC1、0xF5-0xFF
}

// 把合法码点编码为 UTF-8 写入 dst（码点已在上游保证合法，恒返回 true）
bool utf8_encode(uint32_t cp, char* dst, size_t* adv) noexcept {
    if (cp < 0x80u) {
        dst[0] = static_cast<char>(cp);
        *adv = 1;
    } else if (cp < 0x800u) {
        dst[0] = static_cast<char>(0xC0u | (cp >> 6));
        dst[1] = static_cast<char>(0x80u | (cp & 0x3Fu));
        *adv = 2;
    } else if (cp < 0x10000u) {
        dst[0] = static_cast<char>(0xE0u | (cp >> 12));
        dst[1] = static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
        dst[2] = static_cast<char>(0x80u | (cp & 0x3Fu));
        *adv = 3;
    } else {
        dst[0] = static_cast<char>(0xF0u | (cp >> 18));
        dst[1] = static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu));
        dst[2] = static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
        dst[3] = static_cast<char>(0x80u | (cp & 0x3Fu));
        *adv = 4;
    }
    return true;
}

}  // namespace

// ===========================================================================
// JsonParser
// ===========================================================================

Err JsonParser::parse(char* buf, size_t len) noexcept {
    buf_ = buf;
    len_ = len;
    count_ = 0;
    root_ = kNone;

    if (len == 0 || len > kMaxInputBytes) return Err::INVALID_JSON;

    size_t pos = 0;
    skip_ws(&pos);
    if (pos >= len_) return Err::INVALID_JSON;

    Err e = parse_value(1, &pos, kNone, &root_);
    if (e != Err::OK) return e;

    skip_ws(&pos);
    if (pos != len_) return Err::INVALID_JSON;  // 根值之后只允许空白
    return Err::OK;
}

void JsonParser::skip_ws(size_t* pos) const noexcept {
    while (*pos < len_ && is_ws(buf_[*pos])) ++*pos;
}

Err JsonParser::new_token(Type t, uint16_t parent, uint16_t* out) noexcept {
    if (count_ >= cfg::JSON_MAX_TOKENS) return Err::INVALID_JSON;
    const uint16_t idx = count_++;
    Token& tk = tokens_[idx];
    tk.type = t;
    tk.parent = parent;
    tk.next = kNone;
    tk.child = kNone;
    tk.v.i64 = 0;  // 同时清零 str.start/str.length
    if (parent != kNone) {
        const Err e = link_child(parent, idx);
        if (e != Err::OK) return e;
    }
    *out = idx;
    return Err::OK;
}

Err JsonParser::link_child(uint16_t parent, uint16_t child) noexcept {
    Token& p = tokens_[parent];
    if (p.child == kNone) {
        p.child = child;
        return Err::OK;
    }
    uint16_t it = p.child;
    while (tokens_[it].next != kNone) it = tokens_[it].next;
    tokens_[it].next = child;
    return Err::OK;
}

Err JsonParser::parse_value(uint32_t depth, size_t* pos, uint16_t parent,
                            uint16_t* out) noexcept {
    if (*pos >= len_) return Err::INVALID_JSON;
    const char c = buf_[*pos];

    switch (c) {
        case '{': {
            if (depth > cfg::JSON_MAX_DEPTH) return Err::INVALID_JSON;
            ++*pos;
            uint16_t t;
            Err e = new_token(Type::Object, parent, &t);
            if (e != Err::OK) return e;
            e = parse_object(depth, pos, t);
            if (e != Err::OK) return e;
            *out = t;
            return Err::OK;
        }
        case '[': {
            if (depth > cfg::JSON_MAX_DEPTH) return Err::INVALID_JSON;
            ++*pos;
            uint16_t t;
            Err e = new_token(Type::Array, parent, &t);
            if (e != Err::OK) return e;
            e = parse_array(depth, pos, t);
            if (e != Err::OK) return e;
            *out = t;
            return Err::OK;
        }
        case '"':
            ++*pos;  // 跳过开引号（写游标从正文起点开始）
            return parse_string(pos, parent, out);
        default:
            if (c == '-' || is_digit(c)) return parse_number(pos, parent, out);
            if (c == 't') return parse_literal(pos, parent, out, Type::Boolean, true, "true");
            if (c == 'f') return parse_literal(pos, parent, out, Type::Boolean, false, "false");
            if (c == 'n') return parse_literal(pos, parent, out, Type::Null, false, "null");
            return Err::INVALID_JSON;
    }
}

Err JsonParser::parse_object(uint32_t depth, size_t* pos, uint16_t obj) noexcept {
    bool first = true;
    for (;;) {
        skip_ws(pos);
        if (*pos >= len_) return Err::INVALID_JSON;

        if (buf_[*pos] == '}') { ++*pos; return Err::OK; }

        if (!first) {
            if (buf_[*pos] != ',') return Err::INVALID_JSON;
            ++*pos;
            skip_ws(pos);
            if (*pos >= len_) return Err::INVALID_JSON;
            if (buf_[*pos] == '}') return Err::INVALID_JSON;  // 尾逗号
        }

        // ---- key ----
        if (buf_[*pos] != '"') return Err::INVALID_JSON;
        ++*pos;
        uint16_t key_tok;
        Err e = parse_string(pos, obj, &key_tok);
        if (e != Err::OK) return e;
        if (is_duplicate_key(obj, key_tok)) return Err::INVALID_JSON;

        // ---- ':' ----
        skip_ws(pos);
        if (*pos >= len_ || buf_[*pos] != ':') return Err::INVALID_JSON;
        ++*pos;
        skip_ws(pos);

        // ---- value ----
        uint16_t val_tok;
        e = parse_value(depth + 1, pos, obj, &val_tok);
        if (e != Err::OK) return e;

        first = false;
    }
}

Err JsonParser::parse_array(uint32_t depth, size_t* pos, uint16_t arr) noexcept {
    bool first = true;
    for (;;) {
        skip_ws(pos);
        if (*pos >= len_) return Err::INVALID_JSON;

        if (buf_[*pos] == ']') { ++*pos; return Err::OK; }

        if (!first) {
            if (buf_[*pos] != ',') return Err::INVALID_JSON;
            ++*pos;
            skip_ws(pos);
            if (*pos >= len_) return Err::INVALID_JSON;
            if (buf_[*pos] == ']') return Err::INVALID_JSON;  // 尾逗号
        }

        uint16_t v;
        const Err e = parse_value(depth + 1, pos, arr, &v);
        if (e != Err::OK) return e;

        first = false;
    }
}

Err JsonParser::parse_string(size_t* pos, uint16_t parent, uint16_t* out) noexcept {
    const size_t raw_start = *pos;  // 解码写入起点（= 原始正文起点）
    size_t w = raw_start;           // 写游标，恒 <= 读游标
    size_t decoded = 0;

    while (*pos < len_) {
        const auto ch = static_cast<unsigned char>(buf_[*pos]);

        if (ch == '"') {
            ++*pos;
            uint16_t t;
            Err e = new_token(Type::String, parent, &t);
            if (e != Err::OK) return e;
            tok(t).v.str.start = static_cast<uint16_t>(raw_start);
            tok(t).v.str.length = static_cast<uint16_t>(decoded);
            *out = t;
            return Err::OK;
        }

        if (ch == '\\') {
            ++*pos;
            if (*pos >= len_) return Err::INVALID_JSON;
            const char esc = buf_[*pos];
            char simple = 0;
            switch (esc) {
                case '"':  simple = '"';  break;
                case '\\': simple = '\\'; break;
                case '/':  simple = '/';  break;
                case 'b':  simple = '\b'; break;
                case 'f':  simple = '\f'; break;
                case 'n':  simple = '\n'; break;
                case 'r':  simple = '\r'; break;
                case 't':  simple = '\t'; break;
                case 'u': {
                    ++*pos;
                    uint32_t cp = 0;
                    Err e = read_hex4(buf_, len_, pos, &cp);
                    if (e != Err::OK) return e;
                    if (cp >= 0xD800u && cp <= 0xDBFFu) {
                        // 高代理项：必须紧跟 \uDC00-\uDFFF
                        if (*pos + 1 >= len_ || buf_[*pos] != '\\' || buf_[*pos + 1] != 'u')
                            return Err::INVALID_JSON;
                        *pos += 2;
                        uint32_t lo = 0;
                        e = read_hex4(buf_, len_, pos, &lo);
                        if (e != Err::OK) return e;
                        if (lo < 0xDC00u || lo > 0xDFFFu) return Err::INVALID_JSON;
                        cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                    } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
                        return Err::INVALID_JSON;  // 孤立低代理项
                    }
                    size_t adv = 0;
                    utf8_encode(cp, buf_ + w, &adv);
                    w += adv;
                    decoded += adv;
                    if (decoded > cfg::JSON_MAX_STR_BYTES) return Err::INVALID_JSON;
                    continue;  // 已处理完整 escape
                }
                default:
                    return Err::INVALID_JSON;  // 非法转义
            }
            ++*pos;
            buf_[w++] = simple;
            ++decoded;
            if (decoded > cfg::JSON_MAX_STR_BYTES) return Err::INVALID_JSON;
            continue;
        }

        if (ch < 0x20u) return Err::INVALID_JSON;  // 裸控制字符

        // UTF-8 序列：严格校验后原样拷贝
        size_t n = 0;
        if (!utf8_valid(buf_ + *pos, len_ - *pos, &n)) return Err::INVALID_JSON;
        for (size_t k = 0; k < n; ++k) buf_[w++] = buf_[(*pos)++];
        decoded += n;
        if (decoded > cfg::JSON_MAX_STR_BYTES) return Err::INVALID_JSON;
    }
    return Err::INVALID_JSON;  // 未闭合字符串
}

Err JsonParser::parse_number(size_t* pos, uint16_t parent, uint16_t* out) noexcept {
    bool neg = false;
    if (buf_[*pos] == '-') { neg = true; ++*pos; }
    if (*pos >= len_ || !is_digit(buf_[*pos])) return Err::INVALID_JSON;

    uint64_t mag = 0;
    if (buf_[*pos] == '0') {
        ++*pos;
        if (*pos < len_ && is_digit(buf_[*pos])) return Err::INVALID_JSON;  // 前导零
    } else {
        const uint64_t limit = neg ? (1ull << 63) : ((1ull << 63) - 1);
        while (*pos < len_ && is_digit(buf_[*pos])) {
            const auto d = static_cast<uint64_t>(buf_[*pos] - '0');
            if (mag > limit / 10 || (mag == limit / 10 && d > limit % 10))
                return Err::INVALID_JSON;  // int64 溢出
            mag = mag * 10 + d;
            ++*pos;
        }
    }

    if (*pos < len_) {
        const char c = buf_[*pos];
        if (c == '.' || c == 'e' || c == 'E') return Err::INVALID_JSON;  // 浮点不支持
        if (!is_value_delim(c)) return Err::INVALID_JSON;
    }

    int64_t v;
    if (neg) {
        v = (mag == (1ull << 63)) ? INT64_MIN : -static_cast<int64_t>(mag);
    } else {
        v = static_cast<int64_t>(mag);
    }

    uint16_t t;
    Err e = new_token(Type::Integer, parent, &t);
    if (e != Err::OK) return e;
    tok(t).v.i64 = v;
    *out = t;
    return Err::OK;
}

Err JsonParser::parse_literal(size_t* pos, uint16_t parent, uint16_t* out,
                              Type t, bool val, const char* word) noexcept {
    for (const char* p = word; *p != '\0'; ++p) {
        if (*pos >= len_ || buf_[*pos] != *p) return Err::INVALID_JSON;
        ++*pos;
    }
    if (*pos < len_ && !is_value_delim(buf_[*pos])) return Err::INVALID_JSON;

    uint16_t h;
    Err e = new_token(t, parent, &h);
    if (e != Err::OK) return e;
    tok(h).v.i64 = val ? 1 : 0;
    *out = h;
    return Err::OK;
}

bool JsonParser::is_duplicate_key(uint16_t obj, uint16_t new_key) const noexcept {
    const Token& nk = tok(new_key);
    uint16_t it = tok(obj).child;
    while (it != kNone) {
        if (it != new_key) {  // 新 key 已挂到链尾但尚无 value，跳过自身
            const Token& k = tok(it);
            if (k.type == Type::String && k.v.str.length == nk.v.str.length &&
                std::memcmp(buf_ + k.v.str.start, buf_ + nk.v.str.start,
                            k.v.str.length) == 0) {
                return true;
            }
        }
        const uint16_t val = tok(it).next;
        it = (val == kNone) ? kNone : tok(val).next;
    }
    return false;
}

// ---- 导航与取值 ----

JsonParser::Type JsonParser::type(Handle h) const noexcept {
    return valid(h) ? tok(h).type : Type::Null;
}

Err JsonParser::member_count(Handle obj, size_t* out) const noexcept {
    if (!valid(obj) || tok(obj).type != Type::Object) {
        *out = 0;
        return Err::INVALID_ARGUMENT;
    }
    size_t n = 0;
    uint16_t it = tok(obj).child;
    while (it != kNone) {
        ++n;
        const uint16_t val = tok(it).next;
        it = (val == kNone) ? kNone : tok(val).next;
    }
    *out = n;
    return Err::OK;
}

Err JsonParser::member_key(Handle obj, size_t index, const char** out,
                           size_t* len) const noexcept {
    if (!valid(obj) || tok(obj).type != Type::Object) return Err::INVALID_ARGUMENT;
    uint16_t it = tok(obj).child;
    for (size_t i = 0; it != kNone; ++i) {
        if (i == index) {
            if (tok(it).type != Type::String) return Err::INTERNAL_ERROR;  // 结构不变量
            *out = buf_ + tok(it).v.str.start;
            *len = tok(it).v.str.length;
            return Err::OK;
        }
        const uint16_t val = tok(it).next;
        it = (val == kNone) ? kNone : tok(val).next;
    }
    return Err::INVALID_ARGUMENT;  // 越界
}

Err JsonParser::member_value(Handle obj, size_t index, Handle* out) const noexcept {
    if (!valid(obj) || tok(obj).type != Type::Object) return Err::INVALID_ARGUMENT;
    uint16_t it = tok(obj).child;
    for (size_t i = 0; it != kNone; ++i) {
        if (i == index) {
            *out = tok(it).next;  // 解析成功不变量：key 后必有 value
            return (*out == kNone) ? Err::INTERNAL_ERROR : Err::OK;
        }
        const uint16_t val = tok(it).next;
        it = (val == kNone) ? kNone : tok(val).next;
    }
    return Err::INVALID_ARGUMENT;  // 越界
}

JsonParser::Handle JsonParser::find(Handle obj, const char* key) const noexcept {
    if (!valid(obj) || tok(obj).type != Type::Object) return kNone;
    const size_t klen = cstr_len(key);
    uint16_t it = tok(obj).child;
    while (it != kNone) {
        const Token& k = tok(it);
        if (k.type == Type::String && k.v.str.length == klen &&
            std::memcmp(buf_ + k.v.str.start, key, klen) == 0) {
            return k.next;
        }
        const uint16_t val = k.next;
        it = (val == kNone) ? kNone : tok(val).next;
    }
    return kNone;
}

bool JsonParser::str_equals(Handle h, const char* s, size_t s_len) const noexcept {
    if (!valid(h) || tok(h).type != Type::String) return false;
    return tok(h).v.str.length == s_len &&
           std::memcmp(buf_ + tok(h).v.str.start, s, s_len) == 0;
}

Err JsonParser::array_length(Handle arr, size_t* out) const noexcept {
    if (!valid(arr) || tok(arr).type != Type::Array) {
        *out = 0;
        return Err::INVALID_ARGUMENT;
    }
    size_t n = 0;
    for (uint16_t it = tok(arr).child; it != kNone; it = tok(it).next) ++n;
    *out = n;
    return Err::OK;
}

Err JsonParser::array_element(Handle arr, size_t index, Handle* out) const noexcept {
    if (!valid(arr) || tok(arr).type != Type::Array) return Err::INVALID_ARGUMENT;
    size_t i = 0;
    for (uint16_t it = tok(arr).child; it != kNone; it = tok(it).next, ++i) {
        if (i == index) {
            *out = it;
            return Err::OK;
        }
    }
    return Err::INVALID_ARGUMENT;  // 越界
}

Err JsonParser::as_string(Handle h, const char** out, size_t* len) const noexcept {
    if (!valid(h) || tok(h).type != Type::String) return Err::INVALID_ARGUMENT;
    if (out != nullptr) *out = buf_ + tok(h).v.str.start;
    if (len != nullptr) *len = tok(h).v.str.length;
    return Err::OK;
}

Err JsonParser::as_i64(Handle h, int64_t* out) const noexcept {
    if (!valid(h) || tok(h).type != Type::Integer) return Err::INVALID_ARGUMENT;
    if (out != nullptr) *out = tok(h).v.i64;
    return Err::OK;
}

Err JsonParser::as_bool(Handle h, bool* out) const noexcept {
    if (!valid(h) || tok(h).type != Type::Boolean) return Err::INVALID_ARGUMENT;
    if (out != nullptr) *out = tok(h).v.i64 != 0;
    return Err::OK;
}

// ===========================================================================
// JsonWriter
// ===========================================================================

void JsonWriter::reset() noexcept {
    len_ = 0;
    fail_ = false;
    depth_ = -1;
    await_value_ = false;
    wrote_root_ = false;
}

bool JsonWriter::fail() noexcept {
    fail_ = true;
    return false;
}

bool JsonWriter::append(char c) noexcept {
    if (fail_) return false;
    if (len_ >= cap_) return fail();
    buf_[len_++] = c;
    return true;
}

bool JsonWriter::append(const char* s, size_t n) noexcept {
    if (fail_) return false;
    if (n > cap_ - len_) return fail();
    for (size_t i = 0; i < n; ++i) buf_[len_++] = s[i];
    return true;
}

bool JsonWriter::write_escaped(const char* s, size_t n) noexcept {
    static const char kHex[] = "0123456789abcdef";
    if (!append('"')) return false;
    for (size_t i = 0; i < n; ++i) {
        const char c = s[i];
        switch (c) {
            case '"':  if (!append("\\\"", 2)) return false; break;
            case '\\': if (!append("\\\\", 2)) return false; break;
            case '\b': if (!append("\\b", 2)) return false; break;
            case '\f': if (!append("\\f", 2)) return false; break;
            case '\n': if (!append("\\n", 2)) return false; break;
            case '\r': if (!append("\\r", 2)) return false; break;
            case '\t': if (!append("\\t", 2)) return false; break;
            case '#':  if (!append("\\u0023", 6)) return false; break;  // 协议强制
            default:
                if (static_cast<unsigned char>(c) < 0x20u) {
                    if (!append("\\u00", 4)) return false;
                    if (!append(kHex[(static_cast<unsigned char>(c) >> 4) & 0xF])) return false;
                    if (!append(kHex[static_cast<unsigned char>(c) & 0xF])) return false;
                } else {
                    if (!append(c)) return false;
                }
                break;
        }
    }
    return append('"');
}

bool JsonWriter::write_i64(int64_t v) noexcept {
    if (fail_) return false;
    char tmp[20];
    size_t n = 0;
    uint64_t m;
    if (v < 0) {
        m = ~static_cast<uint64_t>(v) + 1;  // INT64_MIN 安全
    } else {
        m = static_cast<uint64_t>(v);
    }
    do {
        tmp[n++] = static_cast<char>('0' + (m % 10));
        m /= 10;
    } while (m != 0);

    const size_t need = n + ((v < 0) ? 1 : 0);
    if (need > cap_ - len_) return fail();
    if (v < 0) buf_[len_++] = '-';
    while (n != 0) buf_[len_++] = tmp[--n];
    return true;
}

bool JsonWriter::begin_element_value() noexcept {
    if (fail_) return false;
    if (await_value_) { await_value_ = false; return true; }  // key 之后的值：无需逗号
    if (depth_ >= 0) {
        if (stack_[depth_].is_object) return fail();  // 对象内取值必须先 key()
        if (!stack_[depth_].first && !append(',')) return false;
        stack_[depth_].first = false;
        return true;
    }
    if (wrote_root_) return fail();  // 根只能有一个值
    wrote_root_ = true;
    return true;
}

bool JsonWriter::key(const char* k) noexcept {
    if (fail_) return false;
    if (await_value_) return fail();                       // 上一个 key 还没有值
    if (depth_ < 0 || !stack_[depth_].is_object) return fail();
    if (!stack_[depth_].first && !append(',')) return false;
    stack_[depth_].first = false;
    if (!write_escaped(k, cstr_len(k))) return false;
    if (!append(':')) return false;
    await_value_ = true;
    return true;
}

bool JsonWriter::begin_object() noexcept {
    if (!begin_element_value()) return false;
    if (depth_ + 1 >= kMaxDepth) return fail();
    if (!append('{')) return false;
    ++depth_;
    stack_[depth_] = Frame{true, true};
    return true;
}

bool JsonWriter::begin_array() noexcept {
    if (!begin_element_value()) return false;
    if (depth_ + 1 >= kMaxDepth) return fail();
    if (!append('[')) return false;
    ++depth_;
    stack_[depth_] = Frame{false, true};
    return true;
}

bool JsonWriter::end_object() noexcept {
    if (fail_) return false;
    if (depth_ < 0 || !stack_[depth_].is_object || await_value_) return fail();
    --depth_;
    return append('}');
}

bool JsonWriter::end_array() noexcept {
    if (fail_) return false;
    if (depth_ < 0 || stack_[depth_].is_object || await_value_) return fail();
    --depth_;
    return append(']');
}

bool JsonWriter::value_string(const char* s, size_t len) noexcept {
    return begin_element_value() && write_escaped(s, len);
}

bool JsonWriter::value_cstr(const char* s) noexcept {
    return value_string(s, cstr_len(s));
}

bool JsonWriter::value_i64(int64_t v) noexcept {
    return begin_element_value() && write_i64(v);
}

bool JsonWriter::value_bool(bool v) noexcept {
    return begin_element_value() && append(v ? "true" : "false", v ? 4 : 5);
}

bool JsonWriter::value_null() noexcept {
    return begin_element_value() && append("null", 4);
}

bool JsonWriter::kv_string(const char* k, const char* v, size_t len) noexcept {
    return key(k) && value_string(v, len);
}

bool JsonWriter::kv_cstr(const char* k, const char* v) noexcept {
    return key(k) && value_cstr(v);
}

bool JsonWriter::kv_i64(const char* k, int64_t v) noexcept {
    return key(k) && value_i64(v);
}

bool JsonWriter::kv_bool(const char* k, bool v) noexcept {
    return key(k) && value_bool(v);
}

bool JsonWriter::kv_null(const char* k) noexcept {
    return key(k) && value_null();
}

bool JsonWriter::terminate() noexcept {
    if (fail_) return false;
    if (len_ >= cap_) return fail();
    buf_[len_] = '\0';
    return true;
}

bool JsonWriter::end_frame() noexcept { return append(cfg::FRAME_TERMINATOR); }

}  // namespace ijpd
