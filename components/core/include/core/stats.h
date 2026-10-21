// ============================================================================
// stats.h —— 打印流水线统计（Phase 4，规范见任务书 7.2/第十章）
//
// 计数项：包计数（接收/解压）、字节数（解压/打印）、CRC/解压失败计数、
//         队列当前水位与历史最大水位、吞吐率（B/s）、任务耗时。
//
// 时间：  由调用方注入毫秒时间戳（平台用 esp_timer；主机测试用假时钟），
//         core 不感知时钟源。
//
// 线程安全性：单计数器原子；snapshot() 为多原子量的非一致快照
//           （观测用途，允许瞬时撕裂）；吞吐窗口 begin/refresh 单写者。
// ============================================================================
#ifndef IJPD_CORE_STATS_H
#define IJPD_CORE_STATS_H

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "core/config_limits.h"

namespace ijpd {

class Stats {
public:
    struct Snapshot {
        uint32_t packets_received = 0;
        uint32_t packets_decoded = 0;
        uint64_t bytes_decoded = 0;
        uint64_t bytes_printed = 0;
        uint32_t crc_errors = 0;
        uint32_t decode_errors = 0;
        size_t queue_watermark = 0;
        size_t queue_watermark_max = 0;
        uint32_t throughput_bps = 0;
        uint64_t elapsed_ms = 0;
    };

    // 任务开始（复位全部计数；now_ms 为任务起始时间）
    void begin(uint64_t now_ms) noexcept;
    void reset() noexcept;

    void on_packet_received() noexcept { packets_received_.fetch_add(1, std::memory_order_relaxed); }
    void on_packet_decoded(uint32_t bytes) noexcept {
        packets_decoded_.fetch_add(1, std::memory_order_relaxed);
        bytes_decoded_.fetch_add(bytes, std::memory_order_relaxed);
        window_bytes_.fetch_add(bytes, std::memory_order_relaxed);
    }
    void on_bytes_printed(uint64_t bytes) noexcept {
        bytes_printed_.fetch_add(bytes, std::memory_order_relaxed);
    }
    void on_crc_error() noexcept { crc_errors_.fetch_add(1, std::memory_order_relaxed); }
    void on_decode_error() noexcept { decode_errors_.fetch_add(1, std::memory_order_relaxed); }
    void on_queue_watermark(size_t busy) noexcept;

    // 滑动吞吐窗口（进度事件周期调用）：窗口 >= PROGRESS_INTERVAL_MS 时
    // 重算吞吐并清零窗口
    void refresh(uint64_t now_ms) noexcept;

    Snapshot snapshot() const noexcept;
    uint64_t start_ms() const noexcept { return start_ms_; }

private:
    std::atomic<uint32_t> packets_received_{0};
    std::atomic<uint32_t> packets_decoded_{0};
    std::atomic<uint64_t> bytes_decoded_{0};
    std::atomic<uint64_t> bytes_printed_{0};
    std::atomic<uint32_t> crc_errors_{0};
    std::atomic<uint32_t> decode_errors_{0};
    std::atomic<size_t> queue_watermark_{0};
    std::atomic<size_t> queue_watermark_max_{0};

    std::atomic<uint32_t> throughput_bps_{0};
    std::atomic<uint64_t> window_bytes_{0};
    uint64_t start_ms_ = 0;      // 单写者（begin）
    uint64_t window_ms_ = 0;     // 单写者（begin/refresh）
};

}  // namespace ijpd

#endif  // IJPD_CORE_STATS_H
