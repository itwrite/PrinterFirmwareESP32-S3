// ============================================================================
// bin_frame_parser.h —— 数据通道二进制分包状态机（Phase 3，协议 §6）
//
// 状态机：WaitHello(12B) → [Header(6B) → Payload(clen) → Crc(4B 可选) →
//                        Dispatch] × total_packets → Done
//
// 校验（协议 §6.3，HEADER 阶段即可判定者先判定）：
//   1. Packet Index 严格顺序（重复/乱序/缺包 -> PACKET_OUT_OF_ORDER）
//   2. Uncompressed Len ∈ [1, 65535]
//   3. none: clen == ulen；lz4_block: clen <= ulen（== 即存储兜底原始数据）
//   4. 非末包 ulen == nominal_packet_bytes；末包 ulen == last_packet_bytes
//   5. integrity=crc32 时 CRC32(6B 头+载荷) 必须匹配
//   6. 累计 ulen == total_bytes_uncompressed（由 4 推出，计数器复核）
//
// 载荷写入调用方提供的外部缓冲（Phase 5 为 PSRAM 压缩缓冲池）：
//   - set_payload_buffer() 必须在载荷字节需要落盘前设置（cap >=
//     MAX_PACKET_COMPRESSED_SIZE）；
//   - 每帧派发后缓冲指针被清空，必须显式重设（防止未消费数据被覆盖）；
//   - 若头部完成后缓冲未就绪且仍有载荷字节待存，feed 返回 NeedBuffer，
//     调用方获取缓冲后从 data+consumed 重喂（粘包安全）。
//
// 线程安全性：非线程安全。单一接收上下文独占（Phase 5 为 rx_task）。
// 错误语义：  任何协议违例 -> 致命，状态进入 Failed 并锁存错误码；
//             后续 feed 恒返回 Failed（字节被吸收丢弃）。
// 最大耗时：  feed O(len)，无动态分配。
// ============================================================================
#ifndef IJPD_CORE_BIN_FRAME_PARSER_H
#define IJPD_CORE_BIN_FRAME_PARSER_H

#include <cstddef>
#include <cstdint>

#include "core/config_limits.h"
#include "core/crc32.h"
#include "core/errors.h"
#include "core/protocol_types.h"

namespace ijpd {

class BinFrameParser {
public:
    enum class Phase { Idle, WaitHello, Header, Payload, Crc, Done, Failed };

    // start_print 派生出的任务级参数（由 CommandRouter::finalize_start_print
    // 保证自洽：nominal*(total-1)+last == total_bytes）
    struct JobConfig {
        uint32_t data_token;
        uint32_t total_packets;           // [1, 65535]
        uint64_t total_bytes_uncompressed;
        uint16_t nominal_packet_bytes;    // >= 1
        uint16_t last_packet_bytes;       // [1, nominal]
        Compression compression;
        Integrity integrity;
    };

    struct Frame {
        uint16_t index;
        uint16_t uncompressed_len;
        uint16_t compressed_len;
        const uint8_t* payload;  // 指向 set_payload_buffer 提供的缓冲
    };

    struct Events {
        void* user = nullptr;
        void (*on_hello_ok)(void* user) = nullptr;
        // 帧派发回调。允许在回调内调用 set_payload_buffer（粘包场景的自然
        // 位置）；禁止在回调内调用 reset()/begin_job()（状态迁移中）。
        void (*on_frame)(void* user, const Frame& f) = nullptr;
    };

    enum class FeedStatus { Ok, NeedBuffer, Failed };

    struct FeedResult {
        FeedStatus status = FeedStatus::Ok;
        Err error = Err::OK;        // Failed：致命错误码；其余 OK
        size_t consumed = 0;        // NeedBuffer：已消费字节数，重喂 data+consumed
        uint32_t frames = 0;        // 本次 feed 派发帧数
    };

    // 配置任务并进入 WaitHello；配置非法返回 INVALID_ARGUMENT（状态不变）
    Err begin_job(const JobConfig& cfg) noexcept;

    // 丢弃任务回到 Idle（任务结束/取消/断连时调用）
    void reset() noexcept;

    // 设置载荷目的缓冲；cap 必须 >= MAX_PACKET_COMPRESSED_SIZE
    Err set_payload_buffer(uint8_t* buf, size_t cap) noexcept;

    // 投喂任意切分的字节流（半包/粘包/随机切块都必须正确）
    FeedResult feed(const uint8_t* data, size_t len, const Events& ev) noexcept;

    Phase phase() const noexcept { return phase_; }
    uint32_t packets_received() const noexcept { return packets_; }
    uint64_t bytes_received() const noexcept { return bytes_; }  // 累计 ulen
    Err last_error() const noexcept { return last_error_; }

    static constexpr size_t kHelloSize = 12;

private:
    Err validate_header_(uint16_t idx, uint16_t ulen, uint16_t clen) const noexcept;
    FeedResult fail_(Err e) noexcept;
    FeedResult ok_(size_t consumed, uint32_t frames) noexcept;

    JobConfig cfg_{};
    Phase phase_ = Phase::Idle;
    Err last_error_ = Err::OK;

    uint8_t hello_[kHelloSize] = {};
    size_t hello_filled_ = 0;

    uint8_t hdr_[6] = {};
    size_t hdr_filled_ = 0;
    uint16_t idx_ = 0, ulen_ = 0, clen_ = 0;

    uint8_t* buf_ = nullptr;
    size_t buf_cap_ = 0;
    size_t payload_filled_ = 0;

    uint32_t crc_state_ = 0;
    uint8_t crc_[4] = {};
    size_t crc_filled_ = 0;

    uint32_t packets_ = 0;
    uint64_t bytes_ = 0;
};

}  // namespace ijpd

#endif  // IJPD_CORE_BIN_FRAME_PARSER_H
