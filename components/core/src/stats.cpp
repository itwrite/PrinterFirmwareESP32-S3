// stats.cpp —— 统计实现
#include "core/stats.h"

namespace ijpd {

void Stats::begin(uint64_t now_ms) noexcept {
    reset();
    start_ms_ = now_ms;
    window_ms_ = now_ms;
}

void Stats::reset() noexcept {
    packets_received_.store(0, std::memory_order_relaxed);
    packets_decoded_.store(0, std::memory_order_relaxed);
    bytes_decoded_.store(0, std::memory_order_relaxed);
    bytes_printed_.store(0, std::memory_order_relaxed);
    crc_errors_.store(0, std::memory_order_relaxed);
    decode_errors_.store(0, std::memory_order_relaxed);
    queue_watermark_.store(0, std::memory_order_relaxed);
    queue_watermark_max_.store(0, std::memory_order_relaxed);
    throughput_bps_.store(0, std::memory_order_relaxed);
    window_bytes_.store(0, std::memory_order_relaxed);
    start_ms_ = 0;
    window_ms_ = 0;
}

void Stats::on_queue_watermark(size_t busy) noexcept {
    queue_watermark_.store(busy, std::memory_order_relaxed);
    // max 水位：CAS 上调
    size_t cur = queue_watermark_max_.load(std::memory_order_relaxed);
    while (busy > cur &&
           !queue_watermark_max_.compare_exchange_weak(cur, busy,
                                                       std::memory_order_relaxed)) {
    }
}

void Stats::refresh(uint64_t now_ms) noexcept {
    const uint64_t elapsed = now_ms - window_ms_;
    if (elapsed >= cfg::PROGRESS_INTERVAL_MS) {
        const uint64_t bytes = window_bytes_.exchange(0, std::memory_order_relaxed);
        // B/s = bytes * 1000 / elapsed（elapsed >= 200ms，乘法无溢出风险）
        const uint32_t bps = static_cast<uint32_t>(bytes * 1000ull / elapsed);
        throughput_bps_.store(bps, std::memory_order_relaxed);
        window_ms_ = now_ms;
    }
}

Stats::Snapshot Stats::snapshot() const noexcept {
    Snapshot s;
    s.packets_received = packets_received_.load(std::memory_order_relaxed);
    s.packets_decoded = packets_decoded_.load(std::memory_order_relaxed);
    s.bytes_decoded = bytes_decoded_.load(std::memory_order_relaxed);
    s.bytes_printed = bytes_printed_.load(std::memory_order_relaxed);
    s.crc_errors = crc_errors_.load(std::memory_order_relaxed);
    s.decode_errors = decode_errors_.load(std::memory_order_relaxed);
    s.queue_watermark = queue_watermark_.load(std::memory_order_relaxed);
    s.queue_watermark_max = queue_watermark_max_.load(std::memory_order_relaxed);
    s.throughput_bps = throughput_bps_.load(std::memory_order_relaxed);
    return s;
}

}  // namespace ijpd
