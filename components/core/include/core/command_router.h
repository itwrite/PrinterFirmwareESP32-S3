// ============================================================================
// command_router.h —— 控制命令路由器（Phase 2）
//
// 处理链（协议 §3/§4）：
//   JSON 帧 → 信封校验(msg_id/cmd/data) → 白名单(UNKNOWN_COMMAND)
//          → schema 参数校验(严格模式：未知字段拒绝)
//          → 命令语义校验(start_print 交叉字段 + 派生量)
//          → 运行时能力门控(UNSUPPORTED)
//          → 应用 hook 产出 result → 统一响应封装（含 '#'）
//
// 响应契约：
//   - 成功：{"msg_id":..,"cmd":..,"data":{"success":true,"result":<hook>,"error":null}}#
//   - 失败：{"msg_id":..,"cmd":..,"data":{"success":false,"result":null,
//            "error":{"code":..,"message":..}}}#
//   - hook 返回错误或序列化溢出时，重置写出器重发完整错误响应；
//     缓冲不足退化为 INTERNAL_ERROR 最小响应（msg_id/cmd 为 null），
//     绝不输出被截断的 JSON。
//
// 线程安全性：非线程安全。单一控制处理上下文独占（Phase 5 为 ctrl_task）。
// 所有权：    frame 为调用方可写缓冲（解析就地改写）；resp 为调用方缓冲。
// 最大耗时：  O(帧长 + 参数数)；hook 必须非阻塞（平台层自行入队）。
// ============================================================================
#ifndef IJPD_CORE_COMMAND_ROUTER_H
#define IJPD_CORE_COMMAND_ROUTER_H

#include <cstddef>
#include <cstdint>

#include "core/config_limits.h"
#include "core/errors.h"
#include "core/json_lite.h"
#include "core/print_profile.h"
#include "core/protocol_types.h"
#include "core/raster_channels.h"

namespace ijpd {

// 数据模式
enum class StartPrintMode : uint8_t { Raster = 0, EscPos = 1 };
const char* start_print_mode_name(StartPrintMode m) noexcept;

enum class StartPrintTransport : uint8_t { Tcp = 0, Mqtt = 1 };

// start_print 已验证参数（schema + 语义校验后；含派生量）
struct StartPrintParams {
    char job_id[cfg::JOB_ID_MAX + 1];  // NUL 结尾
    uint64_t total_bytes_uncompressed;
    uint32_t total_packets;
    Compression compression;
    Integrity integrity;
    StartPrintMode mode = StartPrintMode::Raster;
    StartPrintTransport transport = StartPrintTransport::Tcp;
    // raster 模式必填
    uint32_t bytes_per_row = 0;
    uint32_t total_rows = 0;
    // 显示还原提示(可选):像素位深与通道平面数(缺省 1/1)
    uint32_t pixel_bits = 1;
    uint32_t image_planes = 1;
    // Optional raster plane order. Empty preserves the legacy positional order.
    uint8_t channel_order_count = 0;
    RasterChannel channel_order[8] = {};
    bool cutter_after_finish = false;
    // 派生量（协议 §4.3）：
    //   nominal = ceil(total_bytes / total_packets)
    //   last    = total_bytes - nominal * (total_packets - 1)，恒在 [1, nominal]
    uint32_t nominal_packet_bytes = 0;
    uint32_t last_packet_bytes = 0;
};

struct CancelPrintParams {
    bool has_job_id;
    char job_id[cfg::JOB_ID_MAX + 1];  // NUL 结尾
};

struct FeedPaperParams {
    uint32_t distance_um;
};

// 运行时能力（平台注入；门控 UNSUPPORTED 与 device_info 的 supported 同源）
struct Capabilities {
    bool print_supported;  // PSRAM 可用等
    bool feed_supported;
    bool cutter_supported;
    bool mqtt_supported = false;
};

class CommandRouter {
public:
    // 应用层 hook：用 w 恰好写出一个 JSON 值（通常是 object）作为 result。
    // 返回非 OK 时整条响应重发为错误（hook 之前的部分写出会被丢弃）。
    struct Hooks {
        void* user = nullptr;
        Err (*get_device_info)(void* user, JsonWriter& w) = nullptr;
        Err (*start_provisioning)(void* user, JsonWriter& w) = nullptr;
        Err (*start_print)(void* user, const StartPrintParams& p, JsonWriter& w) = nullptr;
        Err (*cancel_print)(void* user, const CancelPrintParams& p, JsonWriter& w) = nullptr;
        Err (*get_print_status)(void* user, JsonWriter& w) = nullptr;
        Err (*feed_paper)(void* user, const FeedPaperParams& p, JsonWriter& w) = nullptr;
        Err (*cut_paper)(void* user, JsonWriter& w) = nullptr;
    };

    CommandRouter() noexcept = default;

    CommandRouter(const CommandRouter&) = delete;
    CommandRouter& operator=(const CommandRouter&) = delete;

    void set_hooks(const Hooks& hooks) noexcept { hooks_ = hooks; }
    void set_capabilities(const Capabilities& caps) noexcept { caps_ = caps; }

    // 中间件：注入打印机 Profile（校验 start_print 参数 / device_info 上报）
    void set_profile(const PrintProfile* profile) noexcept { profile_ = profile; }
    const PrintProfile* profile() const noexcept { return profile_; }

    // 最近一次校验失败的具体消息（英文，用于日志；响应中同步携带）
    const char* last_error_message() const noexcept { return err_msg_; }

    // 处理一帧 JSON 正文（不含 '#'；会被就地改写）。
    // 响应写入 resp（含结尾 '#'），长度 *resp_len。
    // 返回响应携带的错误码（OK = success:true）。resp 为平台提供的
    // cfg::MAX_RESPONSE_FRAME_SIZE 字节缓冲。
    Err handle(char* frame, size_t frame_len,
               char* resp, size_t resp_cap, size_t* resp_len) noexcept;

    // start_print 交叉字段校验 + 派生量计算（Profile 驱动；主机测试可直接调用）
    static Err finalize_start_print(StartPrintParams* p, const PrintProfile* profile,
                                    const char** msg) noexcept;

    // 传输层错误响应（msg_id/cmd 为 null）：拆帧阶段发现的 FRAME_TOO_LARGE /
    // FRAME_TIMEOUT 等无法进入 handle() 的错误使用本接口。
    // 成功写入返回响应码并置 *resp_len；缓冲不足返回 INTERNAL_ERROR（resp_len=0）。
    static Err respond_error(char* resp, size_t resp_cap, size_t* resp_len,
                             Err code, const char* message) noexcept;

private:
    // 信封三元组
    struct Envelope {
        const char* msg_id = nullptr;
        size_t msg_id_len = 0;
        const char* cmd = nullptr;
        size_t cmd_len = 0;
        JsonParser::Handle data = JsonParser::kNone;
    };

    Err validate_envelope(Envelope* env) noexcept;
    Err validate_data_against_schema(JsonParser::Handle data,
                                     const struct CommandSpec& spec) noexcept;
    void set_msg(const char* key, size_t key_len, const char* what) noexcept;
    void set_msg_key(const char* key, const char* what) noexcept;  // C 字符串键
    void set_msg_raw(const char* what) noexcept;

    Err dispatch(const struct CommandSpec& spec, const Envelope& env,
                 char* resp, size_t resp_cap, size_t* resp_len) noexcept;
    Err internal_fail(const Envelope& env, char* resp, size_t resp_cap,
                      size_t* resp_len) noexcept;

    JsonParser parser_;
    Hooks hooks_{};
    Capabilities caps_{false, false, false};
    const PrintProfile* profile_ = nullptr;  // 中间件 Profile（可为 null = 无约束）
    char err_msg_[192] = {};
};

}  // namespace ijpd

#endif  // IJPD_CORE_COMMAND_ROUTER_H
