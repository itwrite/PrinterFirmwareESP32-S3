// command_router.cpp —— 信封/schema/语义校验、能力门控、分发与响应封装
#include "core/command_router.h"

#include <cstring>

#include "core/command_schema.h"

namespace ijpd {
namespace {

using Handle = JsonParser::Handle;
using Type = JsonParser::Type;

bool idsafe(const char* s, size_t n) noexcept {
    for (size_t i = 0; i < n; ++i) {
        const char c = s[i];
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

// ---- 响应封装 ----

bool write_envelope_head(JsonWriter& w, const char* msg_id, size_t msg_id_len,
                         const char* cmd, size_t cmd_len, bool success) noexcept {
    if (!w.begin_object()) return false;
    if (msg_id != nullptr ? !w.kv_string("msg_id", msg_id, msg_id_len)
                          : !w.kv_null("msg_id")) {
        return false;
    }
    if (cmd != nullptr ? !w.kv_string("cmd", cmd, cmd_len) : !w.kv_null("cmd")) return false;
    if (!w.key("data") || !w.begin_object()) return false;
    return w.kv_bool("success", success);
}

bool write_error_tail(JsonWriter& w, Err code, const char* message) noexcept {
    if (!w.key("error") || !w.begin_object()) return false;
    if (!w.kv_cstr("code", err_name(code))) return false;
    if (!w.kv_cstr("message", (message != nullptr && message[0] != '\0')
                                  ? message
                                  : err_default_message(code))) {
        return false;
    }
    return w.end_object() && w.end_object() && w.end_object() && w.end_frame();
}

// 失败响应（msg_id/cmd 可为 nullptr -> null）
Err emit_error(char* resp, size_t cap, size_t* resp_len,
               const char* msg_id, size_t msg_id_len,
               const char* cmd, size_t cmd_len,
               Err code, const char* message) noexcept {
    JsonWriter w(resp, cap);
    if (write_envelope_head(w, msg_id, msg_id_len, cmd, cmd_len, false) &&
        w.kv_null("result") && write_error_tail(w, code, message)) {
        *resp_len = w.length();
        return code;
    }
    // 理论不可达（缓冲 >= ~160B 即可）；保底：空响应由传输层丢弃
    *resp_len = 0;
    return Err::INTERNAL_ERROR;
}

// 成功响应：hook 写 result 值
Err emit_success(char* resp, size_t cap, size_t* resp_len,
                 const char* msg_id, size_t msg_id_len,
                 const char* cmd, size_t cmd_len,
                 Err (*hook)(void*, JsonWriter&), void* user) noexcept {
    JsonWriter w(resp, cap);
    if (write_envelope_head(w, msg_id, msg_id_len, cmd, cmd_len, true) &&
        w.key("result")) {
        const Err he = (hook != nullptr) ? hook(user, w) : Err::INTERNAL_ERROR;
        if (he == Err::OK && w.ok() && w.kv_null("error") &&
            w.end_object() && w.end_object() && w.end_frame()) {
            *resp_len = w.length();
            return Err::OK;
        }
        if (he != Err::OK) {
            // hook 逻辑失败：重发完整错误响应（丢弃半成品）
            return emit_error(resp, cap, resp_len, msg_id, msg_id_len, cmd, cmd_len,
                              he, nullptr);
        }
    }
    // 序列化溢出：退化为 INTERNAL_ERROR 最小响应（msg_id/cmd 置 null，保证可写）
    static const char* kOverflowMsg = "response serialization failed";
    return emit_error(resp, cap, resp_len, nullptr, 0, nullptr, 0,
                      Err::INTERNAL_ERROR, kOverflowMsg);
}

template <typename P>
Err emit_success_typed(char* resp, size_t cap, size_t* resp_len,
                       const char* msg_id, size_t msg_id_len,
                       const char* cmd, size_t cmd_len,
                       Err (*hook)(void*, const P&, JsonWriter&), const P& p,
                       void* user) noexcept {
    // 适配为无参 hook 原型
    struct Adapter {
        Err (*hook)(void*, const P&, JsonWriter&);
        const P& p;
        void* user;
        static Err call(void* self_, JsonWriter& w) noexcept {
            auto* self = static_cast<Adapter*>(self_);
            return self->hook(self->user, self->p, w);
        }
    };
    Adapter ad{hook, p, user};
    return emit_success(resp, cap, resp_len, msg_id, msg_id_len, cmd, cmd_len,
                        &Adapter::call, &ad);
}

bool cmd_is(const CommandSpec& s, const char* name) noexcept {
    return std::strcmp(s.cmd, name) == 0;
}

}  // namespace

const char* start_print_mode_name(StartPrintMode m) noexcept {
    return (m == StartPrintMode::EscPos) ? "escpos" : "raster";
}

// ===========================================================================
// 公开入口
// ===========================================================================

Err CommandRouter::handle(char* frame, size_t frame_len,
                          char* resp, size_t resp_cap, size_t* resp_len) noexcept {
    err_msg_[0] = '\0';

    Err e = parser_.parse(frame, frame_len);
    if (e != Err::OK) {
        set_msg_raw("malformed JSON frame");
        return emit_error(resp, resp_cap, resp_len, nullptr, 0, nullptr, 0,
                          Err::INVALID_JSON, err_msg_);
    }

    Envelope env;
    e = validate_envelope(&env);
    if (e != Err::OK) {
        return emit_error(resp, resp_cap, resp_len, env.msg_id, env.msg_id_len,
                          env.cmd, env.cmd_len, e, err_msg_);
    }

    const CommandSpec* spec = find_command(env.cmd, env.cmd_len);
    if (spec == nullptr) {
        set_msg_raw("command is not in the whitelist");
        return emit_error(resp, resp_cap, resp_len, env.msg_id, env.msg_id_len,
                          env.cmd, env.cmd_len, Err::UNKNOWN_COMMAND, err_msg_);
    }

    e = validate_data_against_schema(env.data, *spec);
    if (e != Err::OK) {
        return emit_error(resp, resp_cap, resp_len, env.msg_id, env.msg_id_len,
                          env.cmd, env.cmd_len, e, err_msg_);
    }

    return dispatch(*spec, env, resp, resp_cap, resp_len);
}

Err CommandRouter::respond_error(char* resp, size_t resp_cap, size_t* resp_len,
                                 Err code, const char* message) noexcept {
    return emit_error(resp, resp_cap, resp_len, nullptr, 0, nullptr, 0, code, message);
}

// ===========================================================================
// 信封校验
// ===========================================================================

Err CommandRouter::validate_envelope(Envelope* env) noexcept {
    const Handle root = parser_.root();
    if (parser_.type(root) != Type::Object) {
        set_msg_raw("request must be a JSON object");
        return Err::INVALID_ARGUMENT;
    }

    size_t mc = 0;
    if (parser_.member_count(root, &mc) != Err::OK || mc != 3) {
        set_msg_raw("request must contain exactly msg_id, cmd and data");
        return Err::INVALID_ARGUMENT;
    }

    const Handle h_msg = parser_.find(root, "msg_id");
    const Handle h_cmd = parser_.find(root, "cmd");
    const Handle h_data = parser_.find(root, "data");
    // mc==3 且三个键都命中 -> 不存在未知顶层字段；否则即缺键或多键
    if (h_msg == JsonParser::kNone || h_cmd == JsonParser::kNone ||
        h_data == JsonParser::kNone) {
        set_msg_raw("request must contain exactly msg_id, cmd and data");
        return Err::INVALID_ARGUMENT;
    }

    const char* s = nullptr;
    size_t n = 0;
    if (parser_.as_string(h_msg, &s, &n) != Err::OK) {
        set_msg_raw("msg_id must be a string");
        return Err::INVALID_ARGUMENT;  // env.msg_id 保持 null
    }
    if (n < 1 || n > cfg::MSG_ID_MAX) {
        set_msg_raw("msg_id must be 1-40 bytes");
        return Err::INVALID_ARGUMENT;
    }
    env->msg_id = s;
    env->msg_id_len = n;

    if (parser_.as_string(h_cmd, &s, &n) != Err::OK) {
        set_msg_raw("cmd must be a string");
        return Err::INVALID_ARGUMENT;
    }
    if (n < 1 || n > 64) {
        set_msg_raw("cmd must be a non-empty string");
        return Err::INVALID_ARGUMENT;
    }
    env->cmd = s;
    env->cmd_len = n;

    if (parser_.type(h_data) != Type::Object) {
        set_msg_raw("data must be an object");
        return Err::INVALID_ARGUMENT;
    }
    env->data = h_data;
    return Err::OK;
}

// ===========================================================================
// schema 参数校验（严格模式：未知字段拒绝）
// ===========================================================================

Err CommandRouter::validate_data_against_schema(Handle data,
                                                const CommandSpec& spec) noexcept {
    // 1) 未知字段检查（严格模式）
    size_t mc = 0;
    if (parser_.member_count(data, &mc) != Err::OK) return Err::INTERNAL_ERROR;
    for (size_t i = 0; i < mc; ++i) {
        const char* key = nullptr;
        size_t key_len = 0;
        if (parser_.member_key(data, i, &key, &key_len) != Err::OK) {
            return Err::INTERNAL_ERROR;
        }
        bool known = false;
        for (uint8_t pi = 0; pi < spec.param_count; ++pi) {
            const size_t plen = std::strlen(spec.params[pi].key);
            if (plen == key_len && std::memcmp(spec.params[pi].key, key, key_len) == 0) {
                known = true;
                break;
            }
        }
        if (!known) {
            set_msg(key, key_len, "is not a recognized field");
            return Err::INVALID_ARGUMENT;
        }
    }

    // 2) 逐参数类型/范围/枚举/长度/字符集
    for (uint8_t pi = 0; pi < spec.param_count; ++pi) {
        const ParamSpec& ps = spec.params[pi];
        const Handle h = parser_.find(data, ps.key);
        if (h == JsonParser::kNone) {
            if (ps.required) {
                set_msg_key(ps.key, "is required");
                return Err::INVALID_ARGUMENT;
            }
            continue;  // 可选缺省
        }

        switch (ps.type) {
            case ParamType::Integer: {
                int64_t v = 0;
                if (parser_.as_i64(h, &v) != Err::OK) {
                    set_msg_key(ps.key, "must be an integer");
                    return Err::INVALID_ARGUMENT;
                }
                if (ps.has_min && v < ps.min_value) {
                    set_msg_key(ps.key, "is below the minimum");
                    return Err::INVALID_ARGUMENT;
                }
                if (ps.has_max && v > ps.max_value) {
                    set_msg_key(ps.key, "is above the maximum");
                    return Err::INVALID_ARGUMENT;
                }
                break;
            }
            case ParamType::Boolean:
                if (parser_.as_bool(h, nullptr) != Err::OK) {
                    set_msg_key(ps.key, "must be a boolean");
                    return Err::INVALID_ARGUMENT;
                }
                break;
            case ParamType::String:
            case ParamType::Enum: {
                const char* v = nullptr;
                size_t vn = 0;
                if (parser_.as_string(h, &v, &vn) != Err::OK) {
                    set_msg_key(ps.key, "must be a string");
                    return Err::INVALID_ARGUMENT;
                }
                if (ps.str_min_len != 0 && vn < ps.str_min_len) {
                    set_msg_key(ps.key, "is too short");
                    return Err::INVALID_ARGUMENT;
                }
                if (ps.str_max_len != 0 && vn > ps.str_max_len) {
                    set_msg_key(ps.key, "is too long");
                    return Err::INVALID_ARGUMENT;
                }
                if (ps.idsafe_charset && !idsafe(v, vn)) {
                    set_msg_key(ps.key, "may only contain [A-Za-z0-9_-]");
                    return Err::INVALID_ARGUMENT;
                }
                if (ps.type == ParamType::Enum) {
                    bool match = false;
                    for (uint8_t oi = 0; oi < ps.enum_option_count; ++oi) {
                        const size_t on = std::strlen(ps.enum_options[oi]);
                        if (on == vn && std::memcmp(ps.enum_options[oi], v, vn) == 0) {
                            match = true;
                            break;
                        }
                    }
                    if (!match) {
                        set_msg_key(ps.key, "is not one of the allowed values");
                        return Err::INVALID_ARGUMENT;
                    }
                }
                break;
            }
            case ParamType::StringArray: {
                if (parser_.type(h) != Type::Array) {
                    set_msg_key(ps.key, "must be an array");
                    return Err::INVALID_ARGUMENT;
                }
                size_t count = 0;
                if (parser_.array_length(h, &count) != Err::OK) {
                    return Err::INTERNAL_ERROR;
                }
                if (ps.has_min && static_cast<int64_t>(count) < ps.min_value) {
                    set_msg_key(ps.key, "has too few items");
                    return Err::INVALID_ARGUMENT;
                }
                if (ps.has_max && static_cast<int64_t>(count) > ps.max_value) {
                    set_msg_key(ps.key, "has too many items");
                    return Err::INVALID_ARGUMENT;
                }
                for (size_t i = 0; i < count; ++i) {
                    Handle item = JsonParser::kNone;
                    if (parser_.array_element(h, i, &item) != Err::OK) {
                        return Err::INTERNAL_ERROR;
                    }
                    const char* value = nullptr;
                    size_t value_len = 0;
                    if (parser_.as_string(item, &value, &value_len) != Err::OK) {
                        set_msg_key(ps.key, "items must be strings");
                        return Err::INVALID_ARGUMENT;
                    }
                    if ((ps.str_min_len != 0 && value_len < ps.str_min_len) ||
                        (ps.str_max_len != 0 && value_len > ps.str_max_len)) {
                        set_msg_key(ps.key, "contains an item with an invalid length");
                        return Err::INVALID_ARGUMENT;
                    }
                }
                break;
            }
            default:
                return Err::INTERNAL_ERROR;
        }
    }
    return Err::OK;
}

// ===========================================================================
// start_print 语义校验 + 派生量
// ===========================================================================

Err CommandRouter::finalize_start_print(StartPrintParams* p,
                                        const PrintProfile* profile,
                                        const char** msg) noexcept {
    *msg = "";
    const uint64_t tb = p->total_bytes_uncompressed;
    const uint64_t tp = p->total_packets;

    // ---- Profile 数据模式检查 ----
    if (profile != nullptr) {
        const DataMode want =
            (p->mode == StartPrintMode::EscPos) ? DataMode::EscPos : DataMode::Raster;
        if (!has_mode(profile->data_modes, want)) {
            *msg = "data mode is not supported by this printer profile";
            return Err::INVALID_ARGUMENT;
        }
        // 压缩选项过滤
        if (p->compression == Compression::Lz4Block && !profile->supports_lz4) {
            *msg = "lz4_block compression is not supported by this printer profile";
            return Err::INVALID_ARGUMENT;
        }
    }

    // ---- raster 模式：行结构校验 ----
    if (p->mode == StartPrintMode::Raster) {
        if (p->bytes_per_row == 0 || p->total_rows == 0) {
            *msg = "bytes_per_row and total_rows are required for mode=raster";
            return Err::INVALID_ARGUMENT;
        }
        // schema 已保证范围（bpr<=65535, rows<=2^26），u64 乘法无溢出
        if (static_cast<uint64_t>(p->bytes_per_row) * p->total_rows != tb) {
            *msg = "bytes_per_row * total_rows must equal total_bytes_uncompressed";
            return Err::INVALID_ARGUMENT;
        }
        if (profile != nullptr) {
            // Profile 宽度约束
            const uint32_t bpr_max = profile->effective_bpr_max(profile->bytes_per_pixel());
            if (p->bytes_per_row > bpr_max) {
                *msg = "bytes_per_row exceeds the printer's maximum row width";
                return Err::INVALID_ARGUMENT;
            }
            if (p->bytes_per_row < profile->bytes_per_row_min) {
                *msg = "bytes_per_row is below the printer's minimum";
                return Err::INVALID_ARGUMENT;
            }
        }
        if (p->channel_order_count != 0 &&
            p->channel_order_count != p->image_planes) {
            *msg = "channel_order length must equal image_planes";
            return Err::INVALID_ARGUMENT;
        }
        for (uint8_t i = 0; i < p->channel_order_count; ++i) {
            for (uint8_t j = 0; j < i; ++j) {
                if (p->channel_order[i] == p->channel_order[j]) {
                    *msg = "channel_order must not contain duplicates";
                    return Err::INVALID_ARGUMENT;
                }
            }
        }
    }

    // ---- 通用：分包与总量 ----
    if (tp > tb) {
        *msg = "total_packets must not exceed total_bytes_uncompressed";
        return Err::INVALID_ARGUMENT;
    }
    if (tb > tp * cfg::MAX_PACKET_UNCOMPRESSED_SIZE) {
        *msg = "total_bytes_uncompressed exceeds total_packets * max packet size";
        return Err::INVALID_ARGUMENT;
    }

    const uint64_t nominal = (tb + tp - 1) / tp;              // ceil
    const uint64_t last = tb - nominal * (tp - 1);
    if (nominal > cfg::MAX_PACKET_UNCOMPRESSED_SIZE || last < 1 || last > nominal) {
        *msg = "internal: derived packet lengths out of range";
        return Err::INTERNAL_ERROR;  // 上面的检查应已排除，理论不可达
    }
    p->nominal_packet_bytes = static_cast<uint32_t>(nominal);
    p->last_packet_bytes = static_cast<uint32_t>(last);
    return Err::OK;
}

// ===========================================================================
// 分发
// ===========================================================================

Err CommandRouter::internal_fail(const Envelope& env, char* resp, size_t resp_cap,
                                 size_t* resp_len) noexcept {
    set_msg_raw("internal: parameter extraction failed");
    return emit_error(resp, resp_cap, resp_len, env.msg_id, env.msg_id_len, env.cmd,
                      env.cmd_len, Err::INTERNAL_ERROR, err_msg_);
}

Err CommandRouter::dispatch(const CommandSpec& spec, const Envelope& env,
                            char* resp, size_t resp_cap, size_t* resp_len) noexcept {
    // ---- 无参数命令 ----
    if (cmd_is(spec, "get_device_info")) {
        if (hooks_.get_device_info == nullptr) {
            set_msg_raw("handler not installed");
            return emit_error(resp, resp_cap, resp_len, env.msg_id, env.msg_id_len,
                              env.cmd, env.cmd_len, Err::INTERNAL_ERROR, err_msg_);
        }
        return emit_success(resp, resp_cap, resp_len, env.msg_id, env.msg_id_len,
                            env.cmd, env.cmd_len, hooks_.get_device_info, hooks_.user);
    }
    if (cmd_is(spec, "get_print_status")) {
        if (hooks_.get_print_status == nullptr) {
            set_msg_raw("handler not installed");
            return emit_error(resp, resp_cap, resp_len, env.msg_id, env.msg_id_len,
                              env.cmd, env.cmd_len, Err::INTERNAL_ERROR, err_msg_);
        }
        return emit_success(resp, resp_cap, resp_len, env.msg_id, env.msg_id_len,
                            env.cmd, env.cmd_len, hooks_.get_print_status, hooks_.user);
    }
    if (cmd_is(spec, "cut_paper")) {
        if (!caps_.cutter_supported) {
            set_msg_raw("cutter is not available on this device");
            return emit_error(resp, resp_cap, resp_len, env.msg_id, env.msg_id_len,
                              env.cmd, env.cmd_len, Err::UNSUPPORTED, err_msg_);
        }
        if (hooks_.cut_paper == nullptr) {
            set_msg_raw("handler not installed");
            return emit_error(resp, resp_cap, resp_len, env.msg_id, env.msg_id_len,
                              env.cmd, env.cmd_len, Err::INTERNAL_ERROR, err_msg_);
        }
        return emit_success(resp, resp_cap, resp_len, env.msg_id, env.msg_id_len,
                            env.cmd, env.cmd_len, hooks_.cut_paper, hooks_.user);
    }

    // ---- start_print ----
    if (cmd_is(spec, "start_print")) {
        StartPrintParams p{};
        {
            const char* s = nullptr;
            size_t n = 0;
            Handle h = parser_.find(env.data, "job_id");
            if (parser_.as_string(h, &s, &n) != Err::OK || n > cfg::JOB_ID_MAX) {
                set_msg_raw("internal: job_id extraction failed");  // 不应发生：schema 已校验
                return emit_error(resp, resp_cap, resp_len, env.msg_id, env.msg_id_len,
                                  env.cmd, env.cmd_len, Err::INTERNAL_ERROR, err_msg_);
            }
            std::memcpy(p.job_id, s, n);
            p.job_id[n] = '\0';

            int64_t v = 0;
            h = parser_.find(env.data, "total_bytes_uncompressed");
            if (parser_.as_i64(h, &v) != Err::OK) return internal_fail(env, resp, resp_cap, resp_len);
            p.total_bytes_uncompressed = static_cast<uint64_t>(v);

            h = parser_.find(env.data, "total_packets");
            if (parser_.as_i64(h, &v) != Err::OK) return internal_fail(env, resp, resp_cap, resp_len);
            p.total_packets = static_cast<uint32_t>(v);

            h = parser_.find(env.data, "bytes_per_row");
            if (parser_.as_i64(h, &v) != Err::OK) return internal_fail(env, resp, resp_cap, resp_len);
            p.bytes_per_row = static_cast<uint32_t>(v);

            h = parser_.find(env.data, "total_rows");
            if (parser_.as_i64(h, &v) != Err::OK) return internal_fail(env, resp, resp_cap, resp_len);
            p.total_rows = static_cast<uint32_t>(v);

            h = parser_.find(env.data, "cutter_after_finish");
            if (h != JsonParser::kNone &&
                parser_.as_bool(h, &p.cutter_after_finish) != Err::OK) {
                return internal_fail(env, resp, resp_cap, resp_len);
            }

            h = parser_.find(env.data, "mode");
            if (h == JsonParser::kNone || parser_.str_equals(h, "raster", 6)) {
                p.mode = StartPrintMode::Raster;
            } else if (parser_.str_equals(h, "escpos", 6)) {
                p.mode = StartPrintMode::EscPos;
            } else {
                return internal_fail(env, resp, resp_cap, resp_len);
            }

            h = parser_.find(env.data, "transport");
            if (h == JsonParser::kNone || parser_.str_equals(h, "tcp", 3)) {
                p.transport = StartPrintTransport::Tcp;
            } else if (parser_.str_equals(h, "mqtt", 4)) {
                p.transport = StartPrintTransport::Mqtt;
            } else {
                return internal_fail(env, resp, resp_cap, resp_len);
            }

            // bytes_per_row / total_rows：schema 层可选，此处按 mode 取值
            h = parser_.find(env.data, "bytes_per_row");
            if (h != JsonParser::kNone && parser_.as_i64(h, &v) == Err::OK) {
                p.bytes_per_row = static_cast<uint32_t>(v);
            }
            h = parser_.find(env.data, "total_rows");
            if (h != JsonParser::kNone && parser_.as_i64(h, &v) == Err::OK) {
                p.total_rows = static_cast<uint32_t>(v);
            }
            // 显示还原提示(可选,缺省 1/1)
            p.pixel_bits = 1;
            p.image_planes = 1;
            bool image_planes_supplied = false;
            h = parser_.find(env.data, "pixel_bits");
            if (h != JsonParser::kNone && parser_.as_i64(h, &v) == Err::OK && v > 0) {
                p.pixel_bits = static_cast<uint32_t>(v);
            }
            h = parser_.find(env.data, "image_planes");
            if (h != JsonParser::kNone) {
                if (parser_.as_i64(h, &v) == Err::OK && v > 0) {
                    image_planes_supplied = true;
                    p.image_planes = static_cast<uint32_t>(v);
                }
            }
            h = parser_.find(env.data, "channel_order");
            p.channel_order_count = 0;
            if (h != JsonParser::kNone) {
                size_t count = 0;
                if (parser_.array_length(h, &count) != Err::OK || count > 8) {
                    return internal_fail(env, resp, resp_cap, resp_len);
                }
                for (size_t i = 0; i < count; ++i) {
                    Handle item = JsonParser::kNone;
                    const char* value = nullptr;
                    size_t value_len = 0;
                    if (parser_.array_element(h, i, &item) != Err::OK ||
                        parser_.as_string(item, &value, &value_len) != Err::OK) {
                        return internal_fail(env, resp, resp_cap, resp_len);
                    }
                    const RasterChannel channel = raster_channel_from_name(value, value_len);
                    if (channel == RasterChannel::Unknown) {
                        set_msg_raw("channel_order contains an unsupported channel");
                        return emit_error(resp, resp_cap, resp_len, env.msg_id, env.msg_id_len,
                                          env.cmd, env.cmd_len, Err::INVALID_ARGUMENT, err_msg_);
                    }
                    p.channel_order[p.channel_order_count++] = channel;
                }
                if (!image_planes_supplied && p.channel_order_count != 0) {
                    p.image_planes = p.channel_order_count;
                }
            }

            h = parser_.find(env.data, "compression");
            if (parser_.str_equals(h, "none", 4)) {
                p.compression = Compression::None;
            } else if (parser_.str_equals(h, "lz4_block", 9)) {
                p.compression = Compression::Lz4Block;
            } else {
                return internal_fail(env, resp, resp_cap, resp_len);  // schema 已校验
            }

            h = parser_.find(env.data, "integrity");
            if (h == JsonParser::kNone || parser_.str_equals(h, "none", 4)) {
                p.integrity = Integrity::None;
            } else if (parser_.str_equals(h, "crc32", 5)) {
                p.integrity = Integrity::Crc32;
            } else {
                return internal_fail(env, resp, resp_cap, resp_len);
            }
        }

        const char* sem_msg = "";
        const Err sem = finalize_start_print(&p, profile_, &sem_msg);
        if (sem != Err::OK) {
            set_msg_raw(sem_msg);
            return emit_error(resp, resp_cap, resp_len, env.msg_id, env.msg_id_len,
                              env.cmd, env.cmd_len, sem, err_msg_);
        }
        if (!caps_.print_supported) {
            set_msg_raw("printing is not available on this device");
            return emit_error(resp, resp_cap, resp_len, env.msg_id, env.msg_id_len,
                              env.cmd, env.cmd_len, Err::UNSUPPORTED, err_msg_);
        }
        if (hooks_.start_print == nullptr) {
            set_msg_raw("handler not installed");
            return emit_error(resp, resp_cap, resp_len, env.msg_id, env.msg_id_len,
                              env.cmd, env.cmd_len, Err::INTERNAL_ERROR, err_msg_);
        }
        return emit_success_typed(resp, resp_cap, resp_len, env.msg_id, env.msg_id_len,
                                  env.cmd, env.cmd_len, hooks_.start_print, p,
                                  hooks_.user);
    }

    // ---- cancel_print ----
    if (cmd_is(spec, "cancel_print")) {
        CancelPrintParams p{false, {0}};
        const Handle h = parser_.find(env.data, "job_id");
        if (h != JsonParser::kNone) {
            const char* s = nullptr;
            size_t n = 0;
            if (parser_.as_string(h, &s, &n) != Err::OK || n > cfg::JOB_ID_MAX) {
                return internal_fail(env, resp, resp_cap, resp_len);
            }
            std::memcpy(p.job_id, s, n);
            p.job_id[n] = '\0';
            p.has_job_id = true;
        }
        if (hooks_.cancel_print == nullptr) {
            set_msg_raw("handler not installed");
            return emit_error(resp, resp_cap, resp_len, env.msg_id, env.msg_id_len,
                              env.cmd, env.cmd_len, Err::INTERNAL_ERROR, err_msg_);
        }
        return emit_success_typed(resp, resp_cap, resp_len, env.msg_id, env.msg_id_len,
                                  env.cmd, env.cmd_len, hooks_.cancel_print, p,
                                  hooks_.user);
    }

    // ---- feed_paper ----
    if (cmd_is(spec, "feed_paper")) {
        FeedPaperParams p{};
        int64_t v = 0;
        const Handle h = parser_.find(env.data, "distance_um");
        if (parser_.as_i64(h, &v) != Err::OK) return internal_fail(env, resp, resp_cap, resp_len);
        p.distance_um = static_cast<uint32_t>(v);

        if (!caps_.feed_supported) {
            set_msg_raw("paper feed is not available on this device");
            return emit_error(resp, resp_cap, resp_len, env.msg_id, env.msg_id_len,
                              env.cmd, env.cmd_len, Err::UNSUPPORTED, err_msg_);
        }
        if (hooks_.feed_paper == nullptr) {
            set_msg_raw("handler not installed");
            return emit_error(resp, resp_cap, resp_len, env.msg_id, env.msg_id_len,
                              env.cmd, env.cmd_len, Err::INTERNAL_ERROR, err_msg_);
        }
        return emit_success_typed(resp, resp_cap, resp_len, env.msg_id, env.msg_id_len,
                                  env.cmd, env.cmd_len, hooks_.feed_paper, p,
                                  hooks_.user);
    }

    return Err::INTERNAL_ERROR;  // 白名单与 dispatch 不一致（结构性错误）
}

// ===========================================================================
// 消息辅助
// ===========================================================================

void CommandRouter::set_msg(const char* key, size_t key_len, const char* what) noexcept {
    size_t i = 0;
    err_msg_[i++] = '\'';
    if (key_len > cfg::JSON_MAX_STR_BYTES) key_len = cfg::JSON_MAX_STR_BYTES;
    std::memcpy(err_msg_ + i, key, key_len);
    i += key_len;
    err_msg_[i++] = '\'';
    err_msg_[i++] = ' ';
    for (const char* p = what; *p != '\0' && i < sizeof(err_msg_) - 1; ++p) {
        err_msg_[i++] = *p;
    }
    err_msg_[i] = '\0';
}

void CommandRouter::set_msg_key(const char* key, const char* what) noexcept {
    set_msg(key, std::strlen(key), what);
}

void CommandRouter::set_msg_raw(const char* what) noexcept {
    size_t i = 0;
    for (const char* p = what; *p != '\0' && i < sizeof(err_msg_) - 1; ++p) {
        err_msg_[i++] = *p;
    }
    err_msg_[i] = '\0';
}

}  // namespace ijpd
