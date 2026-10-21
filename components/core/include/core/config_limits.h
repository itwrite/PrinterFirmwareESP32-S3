// ============================================================================
// config_limits.h —— 全部容量上限/端口/超时集中定义（Phase 1）
//
// 定稿依据：docs/protocol_v1.md §8、docs/memory_budget.md。
// 修改本文件属于协议/资源变更，必须同步修订上述文档并升级版本。
//
// 线程安全性：纯 constexpr 常量，线程安全。
// 注意：core 层禁止依赖 ESP-IDF；此处一律使用 constexpr，不用 sdkconfig 宏。
// ============================================================================
#ifndef IJPD_CORE_CONFIG_LIMITS_H
#define IJPD_CORE_CONFIG_LIMITS_H

#include <cstddef>
#include <cstdint>

namespace ijpd {
namespace cfg {

// ---------------- 网络端口与连接数 ----------------
inline constexpr uint16_t CONTROL_TCP_PORT   = 9100;  // TCP 控制通道
inline constexpr uint16_t DATA_TCP_PORT      = 9101;  // TCP 数据通道
inline constexpr uint16_t MAX_CTRL_TCP_CONNS = 2;     // TCP 控制连接上限

// ---------------- BLE ----------------
inline constexpr uint16_t BLE_MTU_TARGET       = 247;  // MTU 协商目标
inline constexpr uint16_t BLE_MIN_NOTIFY_CHUNK = 20;   // 分片下限（MTU-3 最小值）

// ---------------- 控制帧 ----------------
inline constexpr char    FRAME_TERMINATOR      = '#';              // 控制帧结束符
inline constexpr size_t  MAX_CONTROL_FRAME_SIZE = 2048;            // 入站 JSON 正文上限（不含 '#'）
inline constexpr size_t  MAX_RESPONSE_FRAME_SIZE = 6144;           // 出站响应帧上限（APP 重组缓冲须 >= 此值）
inline constexpr uint32_t CTRL_FRAME_TIMEOUT_MS = 3000;            // 半帧重组超时（BLE/TCP 通用）

// ---------------- JSON 子集 ----------------
inline constexpr size_t   JSON_MAX_TOKENS    = 64;    // 入站解析 token 数组容量（固定，零堆分配）
inline constexpr uint32_t JSON_MAX_DEPTH     = 4;     // 入站嵌套深度上限（出站写出器不受此限）
inline constexpr size_t   JSON_MAX_STR_BYTES = 128;   // 单个字符串解码后最大字节数
inline constexpr size_t   MSG_ID_MAX         = 40;    // msg_id 长度上限
inline constexpr size_t   JOB_ID_MAX         = 40;    // job_id 长度上限

// ---------------- 二进制分包 ----------------
inline constexpr size_t   MAX_PACKET_UNCOMPRESSED_SIZE = 65535;  // Uncompressed Len 字段上限
inline constexpr size_t   MAX_PACKET_COMPRESSED_SIZE   = 65535;  // Compressed Len 字段上限
inline constexpr uint64_t MAX_JOB_BYTES                = 64ull * 1024 * 1024;  // 64 MiB
inline constexpr uint32_t MAX_TOTAL_PACKETS            = 65535;  // Packet Index 16 位上限

// ---------------- 槽位池 / 压缩缓冲 ----------------
inline constexpr size_t SLOT_SIZE            = 65536;  // 槽位分配大小（含对齐余量，>= MAX_PACKET_UNCOMPRESSED_SIZE）
inline constexpr size_t SLOT_COUNT           = 6;      // 槽位数量
inline constexpr size_t COMPRESSED_BUF_SIZE  = 65536;  // 压缩接收缓冲大小
inline constexpr size_t COMPRESSED_BUF_COUNT = 2;      // rx↔decode 乒乓缓冲数量

// ---------------- 流控水位（按 READY+PROCESSING 槽位计数） ----------------
inline constexpr size_t HIGH_WATERMARK = 4;  // 达到则暂停读取数据 socket（TCP 背压）
inline constexpr size_t LOW_WATERMARK  = 2;  // 回落则恢复读取（滞回防抖）

// ---------------- 超时 ----------------
inline constexpr uint32_t DATA_CONNECT_TIMEOUT_MS = 10000;    // start_print 后等待数据连接握手
inline constexpr uint32_t PACKET_IDLE_TIMEOUT_MS  = 15000;    // 数据包间空闲(高 RTT/拥塞 Wi-Fi 下 5s 过紧)
inline constexpr uint32_t JOB_TOTAL_TIMEOUT_MS    = 600000;   // 任务总超时

// ---------------- 进度事件 ----------------
inline constexpr uint32_t PROGRESS_INTERVAL_MS     = 200;  // 周期上报间隔
inline constexpr uint32_t PROGRESS_EVERY_PACKETS   = 10;   // 或每 N 包上报（先到者）

// ---------------- 协议版本 ----------------
inline constexpr uint16_t PROTOCOL_HELLO_VERSION = 0x0001;  // 数据通道 Hello Version 字段
inline constexpr const char* PROTOCOL_VERSION    = "1.0";   // get_device_info.protocol_version

// ---------------- 自洽性检查（编译期强制） ----------------
static_assert(SLOT_SIZE >= MAX_PACKET_UNCOMPRESSED_SIZE,
              "slot must hold the largest uncompressed packet");
static_assert(SLOT_COUNT >= 2 && SLOT_COUNT <= 16, "slot count out of sane range");
static_assert(HIGH_WATERMARK <= SLOT_COUNT, "high watermark exceeds slot count");
static_assert(LOW_WATERMARK < HIGH_WATERMARK, "low watermark must be < high watermark");
static_assert(COMPRESSED_BUF_SIZE >= MAX_PACKET_COMPRESSED_SIZE,
              "compressed buffer must hold the largest compressed packet");
static_assert(COMPRESSED_BUF_COUNT >= 1, "need at least one compressed buffer");
static_assert(MAX_JOB_BYTES <= static_cast<uint64_t>(MAX_TOTAL_PACKETS) * MAX_PACKET_UNCOMPRESSED_SIZE,
              "job size not representable by (packets x packet size)");
static_assert(MAX_PACKET_UNCOMPRESSED_SIZE <= 65535, "length field is u16");
static_assert(MAX_PACKET_COMPRESSED_SIZE <= 65535, "length field is u16");
static_assert(MAX_CONTROL_FRAME_SIZE <= 65534, "json token offsets are u16");
static_assert(MAX_RESPONSE_FRAME_SIZE >= MAX_CONTROL_FRAME_SIZE + 1024,
              "response buffer must exceed inbound frame (device_info is large)");
static_assert(JSON_MAX_DEPTH >= 1, "depth limit must allow root");
static_assert(JSON_MAX_TOKENS >= 8, "token array too small for any useful object");
static_assert(MSG_ID_MAX <= JSON_MAX_STR_BYTES && JOB_ID_MAX <= JSON_MAX_STR_BYTES,
              "id limits must fit string limit");

}  // namespace cfg
}  // namespace ijpd

#endif  // IJPD_CORE_CONFIG_LIMITS_H
