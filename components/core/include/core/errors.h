// ============================================================================
// errors.h —— 统一错误码（Phase 1）
//
// 线程安全性：err_name() 只读，线程安全；Result 为纯值类型。
// 所有权：    无资源持有。
// 容量上限：  无。
// 最大耗时：  err_name() 一次 switch，O(1)。
//
// 错误码集合与 docs/protocol_v1.md §3.3 一一对应（23 条协议错误码 + OK）。
// err_name() 是全系统唯一“码 → 字符串”映射，JSON error.code 字段直接使用其返回值。
// ============================================================================
#ifndef IJPD_CORE_ERRORS_H
#define IJPD_CORE_ERRORS_H

#include <cstdint>

namespace ijpd {

enum class Err : int32_t {
    OK = 0,
    INVALID_JSON = 1,            // JSON 语法/约束违反（深度、字符串长度、非法转义、非法 UTF-8 等）
    INVALID_ARGUMENT = 2,        // 参数类型/范围/组合非法，或未知字段
    UNKNOWN_COMMAND = 3,         // cmd 不在白名单
    FRAME_TOO_LARGE = 4,         // 控制帧超过 MAX_CONTROL_FRAME_SIZE
    FRAME_TIMEOUT = 5,           // 控制帧重组超时
    PACKET_OUT_OF_ORDER = 6,     // 分包序号重复/乱序/缺失（V1 严格顺序模式）
    PACKET_LENGTH_INVALID = 7,   // 分包长度字段非法或累计字节数不符
    PACKET_CRC_MISMATCH = 8,     // CRC32 校验失败
    DECOMPRESSION_FAILED = 9,    // LZ4 流损坏
    DECOMPRESSED_LENGTH_MISMATCH = 10,  // 解压输出长度 != expected_len
    QUEUE_TIMEOUT = 11,          // 等待槽位/缓冲超时
    PSRAM_UNAVAILABLE = 12,      // PSRAM 未初始化或容量不足
    OUT_OF_MEMORY = 13,          // 内存分配失败
    JOB_ALREADY_RUNNING = 14,    // 已有活动任务
    NO_ACTIVE_JOB = 15,          // 无活动任务时执行了需要任务的命令
    DATA_CONNECTION_LOST = 16,   // 数据连接意外断开
    DATA_HANDSHAKE_FAILED = 17,  // 数据连接握手失败（魔数/版本/令牌/保留字段）
    HAL_NOT_CONFIGURED = 18,     // HAL 未配置
    HAL_FAULT = 19,              // HAL 故障（含注入故障）
    UNSUPPORTED = 20,            // 设备不支持该能力
    JOB_CANCELLED = 21,          // 任务被取消
    JOB_TIMEOUT = 22,            // 任务超时
    INTERNAL_ERROR = 23,         // 内部错误（含响应序列化溢出）

    // ---- 以下为内部错误码：仅在组件间使用，绝不序列化进协议 error.code ----
    STALE_GENERATION = 100,      // 槽位提交时代次已过期（任务已取消/致命终结）
};

// 唯一“码 → 字符串”映射（与协议 error.code 完全一致）
const char* err_name(Err e) noexcept;

// 错误码默认英文文案（error.message 兜底值；调用方可用更具体的消息覆盖）
const char* err_default_message(Err e) noexcept;

inline bool ok(Err e) noexcept { return e == Err::OK; }

// 轻量 Result：不使用异常、不用裸 bool 掩盖错误原因
template <typename T>
struct Result {
    Err err;
    T value {};

    bool is_ok() const noexcept { return err == Err::OK; }

    static Result make(T v) noexcept { return Result{Err::OK, static_cast<T&&>(v)}; }
    static Result fail(Err e) noexcept { return Result{e, T{}}; }
};

template <>
struct Result<void> {
    Err err;

    bool is_ok() const noexcept { return err == Err::OK; }

    static Result<void> make() noexcept { return Result<void>{Err::OK}; }
    static Result<void> fail(Err e) noexcept { return Result<void>{e}; }
};

}  // namespace ijpd

#endif  // IJPD_CORE_ERRORS_H
