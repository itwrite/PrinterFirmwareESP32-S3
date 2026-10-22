// command_schema.cpp —— 六命令静态表（单一数据源）
#include "core/command_schema.h"

#include <cstring>

#include "core/config_limits.h"
#include "core/json_lite.h"

namespace ijpd {
namespace {

// ---- 构造辅助（constexpr，避免 18 字段位置初始化笔误） ----
constexpr ParamSpec p_int(const char* key, const char* label, const char* desc,
                          bool required, int64_t mn, int64_t mx, const char* unit) noexcept {
    ParamSpec s{};
    s.key = key; s.label = label; s.description = desc;
    s.type = ParamType::Integer; s.required = required;
    s.unit = unit; s.has_min = true; s.has_max = true;
    s.min_value = mn; s.max_value = mx;
    return s;
}

constexpr ParamSpec p_str(const char* key, const char* label, const char* desc,
                          bool required, uint16_t mn, uint16_t mx, bool idsafe) noexcept {
    ParamSpec s{};
    s.key = key; s.label = label; s.description = desc;
    s.type = ParamType::String; s.required = required;
    s.str_min_len = mn; s.str_max_len = mx; s.idsafe_charset = idsafe;
    return s;
}

constexpr ParamSpec p_bool(const char* key, const char* label, const char* desc,
                           bool required, bool def) noexcept {
    ParamSpec s{};
    s.key = key; s.label = label; s.description = desc;
    s.type = ParamType::Boolean; s.required = required;
    s.has_default = true; s.default_int = def ? 1 : 0;
    return s;
}

constexpr ParamSpec p_enum(const char* key, const char* label, const char* desc,
                           bool required, const char* const* opts, uint8_t opt_count,
                           const char* def) noexcept {
    ParamSpec s{};
    s.key = key; s.label = label; s.description = desc;
    s.type = ParamType::Enum; s.required = required;
    s.enum_options = opts; s.enum_option_count = opt_count;
    if (def != nullptr) { s.has_default = true; s.default_str = def; }
    return s;
}

constexpr ParamSpec p_string_array(const char* key, const char* label, const char* desc,
                                   bool required, int64_t min_count,
                                   int64_t max_count) noexcept {
    ParamSpec s{};
    s.key = key; s.label = label; s.description = desc;
    s.type = ParamType::StringArray; s.required = required;
    s.has_min = true; s.has_max = true;
    s.min_value = min_count; s.max_value = max_count;
    s.str_min_len = 1; s.str_max_len = 32;
    return s;
}

// ---- 枚举候选 ----
constexpr const char* const kCompressionOpts[] = {"none", "lz4_block"};
constexpr const char* const kIntegrityOpts[] = {"none", "crc32"};

// ---- 参数表 ----
constexpr const char* const kModeOpts[] = {"raster", "escpos"};

constexpr ParamSpec kStartPrintParams[] = {
    p_str("job_id", "任务 ID", "本次打印任务的标识，仅允许 [A-Za-z0-9_-]",
          true, 1, static_cast<uint16_t>(cfg::JOB_ID_MAX), true),
    p_int("total_bytes_uncompressed", "解压后总字节数", "本任务全部解压后数据的预期总长度",
          true, 1, static_cast<int64_t>(cfg::MAX_JOB_BYTES), "bytes"),
    p_int("total_packets", "总包数", "数据分包总数",
          true, 1, static_cast<int64_t>(cfg::MAX_TOTAL_PACKETS), nullptr),
    p_enum("compression", "压缩算法", "none=不压缩；lz4_block=独立 LZ4 Block（含存储兜底规则）",
           true, kCompressionOpts, 2, nullptr),
    p_enum("integrity", "校验算法", "crc32 时每包帧尾附带 4 字节 CRC32（覆盖帧头+载荷）",
           false, kIntegrityOpts, 2, "none"),
    p_enum("mode", "数据模式", "raster=结构化栅格行；escpos=ESC/POS 原始流透传",
           false, kModeOpts, 2, "raster"),
    // bytes_per_row / total_rows 仅 mode=raster 必填（schema 层可选，语义层按 mode 校验）
    p_int("bytes_per_row", "每行字节数", "一行像素数据的字节数（mode=raster 时必填）",
          false, 1, 65535, "bytes"),
    p_int("total_rows", "总行数", "打印总行数（mode=raster 时必填）",
          false, 1, static_cast<int64_t>(cfg::MAX_JOB_BYTES), nullptr),
    // 显示还原提示(可选):0 表示缺省(像素位深 1、平面数 1),语义层归一化
    p_int("pixel_bits", "像素位深", "还原显示用:1=1-bit,8=8-bit(缺省 1)",
          false, 0, 8, nullptr),
    p_int("image_planes", "通道平面数", "还原显示用:光栅平面数(缺省 1)",
           false, 0, 8, nullptr),
    p_string_array("channel_order", "通道顺序", "还原显示用:每个光栅平面的通道名",
                   false, 1, 8),
    p_bool("cutter_after_finish", "完成后切纸", "设备不支持切纸时任务以警告完成",
           false, false),
};

constexpr ParamSpec kCancelPrintParams[] = {
    p_str("job_id", "任务 ID", "要取消的任务；缺省取消当前任务",
          false, 1, static_cast<uint16_t>(cfg::JOB_ID_MAX), true),
};

constexpr ParamSpec kFeedPaperParams[] = {
    p_int("distance_um", "走纸距离", "走纸距离，单位微米",
          true, 1, 254000, "um"),
};

// ---- 命令表（顺序即 device_info 输出顺序） ----
constexpr CommandSpec kCommands[] = {
    {"get_device_info", "获取设备信息",
     "查询设备、能力、限制与命令元数据", nullptr, 0},
    {"start_print", "开始打印",
     "创建打印任务并初始化有界缓存", kStartPrintParams,
     static_cast<uint8_t>(sizeof(kStartPrintParams) / sizeof(kStartPrintParams[0]))},
    {"cancel_print", "取消打印",
     "取消当前或指定任务", kCancelPrintParams,
     static_cast<uint8_t>(sizeof(kCancelPrintParams) / sizeof(kCancelPrintParams[0]))},
    {"get_print_status", "查询打印状态",
     "查询当前/最近任务状态与统计", nullptr, 0},
    {"feed_paper", "走纸",
     "走纸指定距离", kFeedPaperParams,
     static_cast<uint8_t>(sizeof(kFeedPaperParams) / sizeof(kFeedPaperParams[0]))},
    {"cut_paper", "切纸",
     "执行切纸（切纸硬件未接入前不支持）", nullptr, 0},
};

constexpr size_t kCommandCount = sizeof(kCommands) / sizeof(kCommands[0]);

}  // namespace

const CommandSpec* find_command(const char* name, size_t name_len) noexcept {
    for (size_t i = 0; i < kCommandCount; ++i) {
        const CommandSpec& c = kCommands[i];
        size_t n = 0;
        while (c.cmd[n] != '\0') ++n;
        if (n == name_len && (name_len == 0 || std::memcmp(c.cmd, name, name_len) == 0)) {
            return &kCommands[i];
        }
    }
    return nullptr;
}

const CommandSpec* command_at(size_t index) noexcept {
    return (index < kCommandCount) ? &kCommands[index] : nullptr;
}

size_t command_count() noexcept { return kCommandCount; }

const char* param_type_name(ParamType t) noexcept {
    switch (t) {
        case ParamType::Integer: return "integer";
        case ParamType::String:  return "string";
        case ParamType::Boolean: return "boolean";
        case ParamType::Enum:    return "enum";
        case ParamType::StringArray: return "string_array";
        default:                 return "unknown";
    }
}

bool write_param_metadata(JsonWriter& w, const ParamSpec& p) noexcept {
    if (!w.begin_object()) return false;
    if (!w.kv_cstr("key", p.key)) return false;
    if (!w.kv_cstr("label", p.label)) return false;
    if (!w.kv_cstr("type", param_type_name(p.type))) return false;
    if (!w.kv_bool("required", p.required)) return false;

    // default_value
    if (!w.key("default_value")) return false;
    if (!p.has_default) {
        if (!w.value_null()) return false;
    } else if (p.type == ParamType::Integer) {
        if (!w.value_i64(p.default_int)) return false;
    } else if (p.type == ParamType::Boolean) {
        if (!w.value_bool(p.default_int != 0)) return false;
    } else {
        if (!w.value_cstr(p.default_str)) return false;
    }

    if (!w.key("options") || !w.begin_array()) return false;
    for (uint8_t i = 0; i < p.enum_option_count; ++i) {
        if (!w.value_cstr(p.enum_options[i])) return false;
    }
    if (!w.end_array()) return false;

    if (p.unit != nullptr ? !w.kv_cstr("unit", p.unit) : !w.kv_null("unit")) return false;

    if (!w.key("min")) return false;
    if (p.has_min ? !w.value_i64(p.min_value) : !w.value_null()) return false;
    if (!w.key("max")) return false;
    if (p.has_max ? !w.value_i64(p.max_value) : !w.value_null()) return false;

    if (!w.kv_cstr("description", p.description)) return false;
    return w.end_object();
}

bool write_command_metadata(JsonWriter& w, const CommandSpec& c, bool supported) noexcept {
    if (!w.begin_object()) return false;
    if (!w.kv_cstr("cmd", c.cmd)) return false;
    if (!w.kv_cstr("label", c.label)) return false;
    if (!w.kv_cstr("description", c.description)) return false;
    if (!w.kv_bool("supported", supported)) return false;
    if (!w.key("params") || !w.begin_array()) return false;
    for (uint8_t i = 0; i < c.param_count; ++i) {
        if (!write_param_metadata(w, c.params[i])) return false;
    }
    if (!w.end_array()) return false;
    return w.end_object();
}

}  // namespace ijpd
