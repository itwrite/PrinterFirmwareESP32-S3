// ============================================================================
// command_schema.h —— 命令元数据单一数据源（Phase 2）
//
// 职责（协议 §4.1/§5.4）：
//   - 六命令白名单与参数约束（类型/必填/范围/枚举/长度/字符集）的唯一事实来源；
//   - command_router 的参数校验 与 device_info 的 commands 元数据输出
//     均遍历同一张表，结构上保证“上报支持的命令 = 实际路由的命令”；
//   - supported 标志是运行时能力（Capabilities），由 device_info/路由注入，
//     不属于静态 schema。
//
// 线程安全性：纯静态只读数据，线程安全。
// 所有权：    所有字符串为静态字面量，无所有权转移。
// ============================================================================
#ifndef IJPD_CORE_COMMAND_SCHEMA_H
#define IJPD_CORE_COMMAND_SCHEMA_H

#include <cstddef>
#include <cstdint>

namespace ijpd {

enum class ParamType : uint8_t { Integer, String, Boolean, Enum, StringArray };

struct ParamSpec {
    const char* key;
    const char* label;         // 中文人读名
    const char* description;
    ParamType type;
    bool required;
    bool has_default;
    int64_t default_int;             // Integer/Boolean 默认值
    const char* default_str;         // String/Enum 默认值
    const char* const* enum_options; // Enum 候选值
    uint8_t enum_option_count;
    const char* unit;                // 可为 nullptr
    bool has_min;
    bool has_max;
    int64_t min_value;
    int64_t max_value;
    uint16_t str_min_len;            // String/Enum 解码后长度下限
    uint16_t str_max_len;            // 上限（0 = 不限）
    bool idsafe_charset;             // 仅允许 [A-Za-z0-9_-]
};

struct CommandSpec {
    const char* cmd;
    const char* label;
    const char* description;
    const ParamSpec* params;
    uint8_t param_count;
};

// 白名单查表（线性）；未找到返回 nullptr
const CommandSpec* find_command(const char* name, size_t name_len) noexcept;

// 全量命令（device_info 遍历用）
const CommandSpec* command_at(size_t index) noexcept;
size_t command_count() noexcept;

const char* param_type_name(ParamType t) noexcept;

// ---------------------------------------------------------------------------
// 元数据写出（与校验共享同一张表；写出器无深度/容量以外的限制）
class JsonWriter;

// 写一条 param 元数据对象：{"key":..,"label":..,"type":..,"required":..,
//   "default_value":..,"options":[..],"unit":..,"min":..,"max":..,"description":..}
bool write_param_metadata(JsonWriter& w, const ParamSpec& p) noexcept;

// 写一条 command 元数据对象：{"cmd":..,"label":..,"description":..,
//   "supported":<运行时传入>,"params":[..]}
bool write_command_metadata(JsonWriter& w, const CommandSpec& c, bool supported) noexcept;

}  // namespace ijpd

#endif  // IJPD_CORE_COMMAND_SCHEMA_H
