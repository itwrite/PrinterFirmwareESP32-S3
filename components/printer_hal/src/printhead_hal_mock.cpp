// hal/printhead_hal_mock.cpp —— Mock 打印头实现（raster + raw 双模式）
#include <cstring>

#include "printer_hal/printhead_hal.h"

namespace ijpd {

Err MockPrintheadHal::begin(const PrintheadConfig& cfg) {
    if (cfg.rows_per_second == 0) {
        return Err::INVALID_ARGUMENT;
    }
    cfg_ = cfg;
    rows_ = 0;
    raw_bytes_ = 0;
    faulted_ = false;
    return Err::OK;
}

Err MockPrintheadHal::submitRaster(const uint8_t* data, size_t len,
                                   const RasterMeta& m) {
    (void)data;
    (void)m;
    if (faulted_) return Err::HAL_FAULT;
    if (len != cfg_.row_bytes) return Err::INVALID_ARGUMENT;

    // Mock 行耗时（真实喷头为硬件时序，Phase 7 实现）
    const uint32_t ms = 1000u / cfg_.rows_per_second;
    if (delay_ != nullptr && ms != 0) delay_(delay_user_, ms);

    ++rows_;
    if (cfg_.fault_after_rows != 0 && rows_ >= cfg_.fault_after_rows) {
        faulted_ = true;
        return Err::HAL_FAULT;
    }
    return Err::OK;
}

Err MockPrintheadHal::submitRaw(const uint8_t* data, size_t len) {
    if (faulted_) return Err::HAL_FAULT;
    if (data == nullptr && len != 0) return Err::INVALID_ARGUMENT;

    // Mock：按块大小模拟耗时（ESC/POS 解释由真实 HAL 实现）
    const uint32_t chunks = static_cast<uint32_t>((len + cfg_.raw_chunk_bytes - 1) /
                                                  cfg_.raw_chunk_bytes);
    const uint32_t ms = chunks;  // 简化：每块 1ms
    if (delay_ != nullptr && ms != 0) delay_(delay_user_, ms);

    raw_bytes_ += len;
    return Err::OK;
}

Err MockPrintheadHal::waitUntilIdle(uint32_t timeout_ms) {
    (void)timeout_ms;  // Mock 同步完成
    return Err::OK;
}

Err MockPrintheadHal::stop() {
    return Err::OK;  // 幂等安全停机
}

}  // namespace ijpd
