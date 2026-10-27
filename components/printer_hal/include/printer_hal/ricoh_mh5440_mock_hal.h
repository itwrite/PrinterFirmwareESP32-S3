// ricoh_mh5440_mock_hal.h - Deterministic logical MH5440 mock.
//
// This class models the CMYK8 row contract and observable job behavior only.
// It does not model Ricoh electrical timing, waveforms, or controller IO.
#ifndef IJPD_PRINTER_HAL_RICOH_MH5440_MOCK_HAL_H
#define IJPD_PRINTER_HAL_RICOH_MH5440_MOCK_HAL_H

#include <cstddef>
#include <cstdint>

#include "core/errors.h"
#include "printer_hal/printhead_hal.h"

namespace ijpd {

struct RicohMh5440MockConfig {
    uint32_t virtual_nozzles_per_channel = 2480;
    uint8_t channel_count = 4;
    uint32_t logical_width_px = 2480;
    uint32_t row_bytes = 0;  // 0 = logical_width_px * channel_count

    bool strict_head_geometry = true;
    bool require_monotonic_rows = true;
    bool allow_raw = false;

    uint32_t row_period_us = 5000;
    uint32_t startup_delay_ms = 0;
    uint32_t stop_delay_ms = 0;

    uint32_t fault_after_rows = 0;
    uint64_t fault_after_bytes = 0;
    uint32_t backpressure_after_rows = 0;
    uint32_t missing_nozzle_ppm = 0;
    uint32_t random_seed = 0x54484D34u;
    bool inject_paper_jam = false;
    bool inject_thermal_fault = false;
};

struct RicohMh5440MockStats {
    uint64_t rows_accepted = 0;
    uint64_t bytes_accepted = 0;
    uint64_t raw_bytes_received = 0;
    uint64_t nonzero_pixels[4] = {};
    uint64_t intensity_sum[4] = {};
    uint64_t missing_nozzle_drops[4] = {};
    uint32_t channel_crc32[4] = {};
    uint32_t last_row_index = 0;
    uint32_t first_bad_row = 0;
    uint32_t fault_count = 0;
    bool has_bad_row = false;
    Err terminal_error = Err::OK;
};

class MockRicohMh5440Hal final : public IPrintheadHal {
public:
    using DelayFn = void (*)(void* user, uint32_t ms);

    explicit MockRicohMh5440Hal(const RicohMh5440MockConfig& config = {},
                                DelayFn delay = nullptr,
                                void* delay_user = nullptr) noexcept
        : config_(config), delay_(delay), delay_user_(delay_user) {}

    Err begin(const PrintheadConfig& cfg) override;
    Err submitRaster(const uint8_t* data, size_t len, const RasterMeta& meta) override;
    Err submitRaw(const uint8_t* data, size_t len) override;
    Err waitUntilIdle(uint32_t timeout_ms) override;
    Err stop() override;
    bool healthy() const override;

    RicohMh5440MockStats stats() const noexcept;
    const RicohMh5440MockConfig& config() const noexcept { return config_; }
    uint32_t row_bytes() const noexcept { return row_bytes_; }
    bool configured() const noexcept { return configured_; }

private:
    Err validate_config_() const noexcept;
    Err fail_input_(Err error, uint32_t row_index) noexcept;
    Err fail_hardware_(Err error) noexcept;
    bool should_drop_(uint32_t row_index, uint32_t pixel, uint32_t channel) noexcept;
    uint32_t next_random_() noexcept;
    void delay_ms_(uint32_t ms) noexcept;
    void reset_stats_() noexcept;

    RicohMh5440MockConfig config_{};
    DelayFn delay_ = nullptr;
    void* delay_user_ = nullptr;
    PrintheadConfig begin_config_{};
    RicohMh5440MockStats stats_{};
    uint32_t channel_crc_state_[4] = {};
    uint32_t random_state_ = 0;
    uint32_t row_bytes_ = 0;
    uint32_t next_row_index_ = 0;
    uint32_t effective_row_period_us_ = 5000;  // begin() 时由 rows_per_second 或 config 推导
    bool configured_ = false;
    bool stopped_ = false;
    bool hardware_fault_ = false;
    bool blocked_ = false;
};

}  // namespace ijpd

#endif  // IJPD_PRINTER_HAL_RICOH_MH5440_MOCK_HAL_H
