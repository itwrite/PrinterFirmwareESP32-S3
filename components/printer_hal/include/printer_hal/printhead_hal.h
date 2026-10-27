// ============================================================================
// printhead_hal.h —— 喷头/打印头抽象接口（中间件 Phase M1 泛化）
//
// V2 变更（中间件化）：
//   - submitRaster：栅格行模式（raster），通用不变
//   - submitRaw  ：原始流模式（ESC/POS 等），按块转发，HAL 自行解释
//   - PrintheadConfig 增加 profile 关联字段
//
// 真实驱动须依据厂商资料实现（Phase 7）。
// ============================================================================
#ifndef IJPD_PRINTER_HAL_PRINTHEAD_HAL_H
#define IJPD_PRINTER_HAL_PRINTHEAD_HAL_H

#include <cstddef>
#include <cstdint>

#include "core/errors.h"
#include "core/print_profile.h"

namespace ijpd {

struct PrintheadConfig {
    uint32_t row_bytes = 0;        // 每行字节数（raster 模式）
    uint32_t rows_per_second = 1;  // Mock：每秒行数
    uint32_t fault_after_rows = 0; // Mock 故障注入：N 行后 HAL_FAULT（0=不注入）
    uint32_t raw_chunk_bytes = 512; // raw 模式每次块大小
    uint32_t rows_per_plane = 0;   // 多遍栅格时每个颜色平面的行数
    uint8_t plane_count = 0;
    uint8_t channel_count = 0;
    uint8_t pixel_bits = 0;
    uint32_t pixel_width = 0;
    RasterLayout raster_layout = RasterLayout::Unknown;
    bool sequential_channels = false;
    bool dispatch_planar_rows = false;
    RasterChannel channel_order[4] = {RasterChannel::Black};
};

struct RasterMeta {
    uint32_t row_index = 0;
    uint8_t plane_index = 0;
};

class IPrintheadHal {
public:
    virtual ~IPrintheadHal() = default;
    virtual Err begin(const PrintheadConfig& cfg) = 0;

    // ---- 栅格行模式（raster） ----
    // 提交一行栅格数据（长度必须 == row_bytes；实现可异步，由 waitUntilIdle 收口）
    virtual Err submitRaster(const uint8_t* data, size_t len, const RasterMeta& m) = 0;

    // ---- 原始流模式（escpos 等） ----
    // 转发一块原始字节流（ESC/POS 命令+数据），HAL 自行解释并执行
    virtual Err submitRaw(const uint8_t* data, size_t len) = 0;

    virtual Err waitUntilIdle(uint32_t timeout_ms) = 0;
    virtual Err stop() = 0;  // 任意状态可安全调用，幂等（安全停机）
    virtual bool healthy() const = 0;
};

// ---- Mock 实现（明确命名；设备侧经 delay 回调注入 vTaskDelay） ----
class MockPrintheadHal final : public IPrintheadHal {
public:
    using DelayFn = void (*)(void* user, uint32_t ms);

    explicit MockPrintheadHal(DelayFn delay = nullptr, void* delay_user = nullptr)
        : delay_(delay), delay_user_(delay_user) {}

    Err begin(const PrintheadConfig& cfg) override;
    Err submitRaster(const uint8_t* data, size_t len, const RasterMeta& m) override;
    Err submitRaw(const uint8_t* data, size_t len) override;
    Err waitUntilIdle(uint32_t timeout_ms) override;
    Err stop() override;
    bool healthy() const override { return !faulted_; }

    uint64_t rows_printed() const { return rows_; }
    uint64_t raw_bytes_received() const { return raw_bytes_; }

private:
    DelayFn delay_;
    void* delay_user_;
    PrintheadConfig cfg_{};
    uint64_t rows_ = 0;
    uint64_t raw_bytes_ = 0;
    bool faulted_ = false;
};

}  // namespace ijpd

#endif  // IJPD_PRINTER_HAL_PRINTHEAD_HAL_H
